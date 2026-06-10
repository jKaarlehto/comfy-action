#include "matrix.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "delivery_event_sink.h"
#include "delivery_types.h"
#include "facade_log.h"
#include "hash_utils.h"
#include "verify/verify_byte_exact.h"
#include "verify/verify_integrity.h"
#include "verify/verify_structural.h"

namespace notch_mock
{

namespace cec = ComfyExtensionClient;

namespace
{

std::vector<std::string> Split(const std::string& csv)
{
    std::vector<std::string> out;
    std::string current;
    std::istringstream stream(csv);
    while (std::getline(stream, current, ','))
    {
        if (!current.empty())
        {
            out.push_back(current);
        }
    }
    return out;
}

std::vector<std::string> Sorted(std::vector<std::string> values)
{
    std::sort(values.begin(), values.end());
    return values;
}

// cuda is image-only; every other output type allows disk and http.
std::vector<std::string> ExpectedTypeTransports(const std::string& type)
{
    if (type == "IMAGE")
    {
        return {"cuda", "disk", "http"};
    }
    return {"disk", "http"};
}

// JSON for an OutputTransportDecision (the facade's negotiation result).
std::string DecisionJson(const cec::OutputTransportDecision& decision)
{
    std::ostringstream json;
    json << "{\"ok\":" << CaseLogger::Bool(decision.ok)
         << ",\"transport\":" << CaseLogger::Quote(decision.transport)
         << ",\"usable\":" << CaseLogger::Array(decision.usable_transports);
    if (!decision.error.empty())
    {
        json << ",\"error\":" << CaseLogger::Quote(decision.error);
    }
    json << "}";
    return json.str();
}

std::string ExpectJson(bool ok, const std::string& transport)
{
    std::ostringstream json;
    json << "{\"ok\":" << CaseLogger::Bool(ok)
         << ",\"transport\":" << CaseLogger::Quote(transport) << "}";
    return json.str();
}

// Extract a top-level JSON string field value, e.g. ExtractJsonStringField(body,
// "ipc_handle") -> the hex. Sufficient for the unescaped hex/string fields the
// share-info endpoint returns; not a general JSON parser.
std::string ExtractJsonStringField(const std::string& body, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    size_t k = body.find(needle);
    if (k == std::string::npos)
    {
        return "";
    }
    size_t colon = body.find(':', k + needle.size());
    if (colon == std::string::npos)
    {
        return "";
    }
    size_t q1 = body.find('"', colon + 1);
    if (q1 == std::string::npos)
    {
        return "";
    }
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos)
    {
        return "";
    }
    return body.substr(q1 + 1, q2 - q1 - 1);
}

// Extract a top-level JSON integer field value, e.g. "cuda_device_index": 0.
// Returns fallback when the field is absent or unparseable.
int ExtractJsonIntField(const std::string& body, const std::string& key, int fallback)
{
    const std::string needle = "\"" + key + "\"";
    size_t k = body.find(needle);
    if (k == std::string::npos)
    {
        return fallback;
    }
    size_t colon = body.find(':', k + needle.size());
    if (colon == std::string::npos)
    {
        return fallback;
    }
    size_t pos = colon + 1;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t'))
    {
        ++pos;
    }
    size_t end = pos;
    if (end < body.size() && (body[end] == '-' || body[end] == '+'))
    {
        ++end;
    }
    size_t digitsStart = end;
    while (end < body.size() && body[end] >= '0' && body[end] <= '9')
    {
        ++end;
    }
    if (end == digitsStart)
    {
        return fallback;
    }
    return std::atoi(body.substr(pos, end - pos).c_str());
}

struct SelectionRow
{
    std::string id;
    std::string title;
    std::string description;
    std::string typeAllowed;
    std::string serverAvailable;
    std::string clientReachable;
    std::string requiredOrPreference; // hard: the single required transport; soft: the csv preference order
    bool expectOk;
    std::string expectTransport;
};

// Each row names the behavioral boundary it proves, not its incidental fixture
// values. The selection helper is purely set-based (usable = typeAllowed n
// serverAvailable n clientReachable; hard request honored iff usable else
// rejected; soft order takes the first usable, else the first usable overall).
// Positives use a maximal fixture (every transport available and reachable) so
// the only thing that varies is the request — proving "requested-and-usable =>
// selected" and that the request is not auto-changed. Negatives isolate the one
// axis that removes the requested transport (type, server, or client), with the
// other axes permissive, so the row proves that axis alone causes the reject.
// 5 positives (cuda + disk/http per type class) + 5 negatives.
const SelectionRow kHardRows[] = {
    // Positives — requested-and-usable => selected (maximal fixture).
    {"image-cuda-usable", "IMAGE output — client requests cuda, gets cuda",
     "Positive cuda boundary for IMAGE. cuda is type-allowed, server-available, and client-reachable, so the hard cuda request is honored. Every transport is usable in the fixture, so cuda being chosen over the also-usable disk/http shows the request is honored as-is, not auto-changed.",
     "cuda,disk,http", "cuda,disk,http", "cuda,disk,http", "cuda", true, "cuda"},
    {"image-disk-usable", "IMAGE output — client requests disk, gets disk",
     "Positive disk boundary for IMAGE. disk is usable, so the hard disk request is honored. The fixture makes cuda usable too, so disk being chosen proves the request wins (no auto-upgrade to cuda). Other transports are incidental.",
     "cuda,disk,http", "cuda,disk,http", "cuda,disk,http", "disk", true, "disk"},
    {"image-http-usable", "IMAGE output — client requests http, gets http",
     "Positive http boundary for IMAGE. http is usable, so the hard http request is honored; the request alone determines the choice.",
     "cuda,disk,http", "cuda,disk,http", "cuda,disk,http", "http", true, "http"},
    {"nonimage-disk-usable", "Non-image output — client requests disk, gets disk",
     "Positive disk boundary for a non-image output (type-allowed = disk/http; cuda is image-only). disk is usable, so the hard disk request is honored. Server and client also offer cuda, but type excludes it — incidental to this positive.",
     "disk,http", "cuda,disk,http", "cuda,disk,http", "disk", true, "disk"},
    {"nonimage-http-usable", "Non-image output — client requests http, gets http",
     "Positive http boundary for a non-image output. http is usable, so the hard http request is honored.",
     "disk,http", "cuda,disk,http", "cuda,disk,http", "http", true, "http"},
    // Negatives — request rejected; each isolates the single excluding axis.
    {"nonimage-cuda-rejected-by-type", "Non-image output — client requests cuda, rejected (cuda is image-only)",
     "Negative cuda boundary isolating the type axis: server and client both offer cuda, but the output type is not IMAGE so cuda is not type-allowed at all. The hard cuda request is rejected — never silently downgraded.",
     "disk,http", "cuda,disk,http", "cuda,disk,http", "cuda", false, ""},
    {"image-cuda-rejected-by-server", "IMAGE output, server has no GPU — client requests cuda, rejected",
     "Negative cuda boundary isolating the server axis: cuda is type-allowed and client-reachable, but the server has no CUDA device, so cuda is not server-available. The hard cuda request is rejected (no downgrade).",
     "cuda,disk,http", "disk,http", "cuda,disk,http", "cuda", false, ""},
    {"image-cuda-rejected-by-client", "IMAGE output, remote client — client requests cuda, rejected",
     "Negative cuda boundary isolating the client axis: cuda is type-allowed and server-available, but the client is remote and cannot receive a host-local GPU share, so cuda is not client-reachable. The hard cuda request is rejected (no downgrade).",
     "cuda,disk,http", "cuda,disk,http", "disk,http", "cuda", false, ""},
    {"nonimage-disk-rejected-by-client", "Non-image output, remote client (http only) — client requests disk, rejected",
     "Negative disk boundary isolating the client axis: disk is type-allowed and server-available, but a remote http-only client has no shared filesystem, so disk is not client-reachable. The hard disk request is rejected (no downgrade).",
     "disk,http", "disk,http", "http", "disk", false, ""},
    {"empty-intersection-fails", "No transport is type-allowed, server-available, and client-reachable — request rejected",
     "Negative boundary where the three eligibility sets do not intersect: the type allows only disk/http, but the server and client offer only cuda. There is no usable transport, so any request fails.",
     "disk,http", "cuda", "cuda", "cuda", false, ""},
};

// Soft preference order is client-owned but part of negotiation; the canonical
// Notch client order is cuda > http > disk (http is always at least as available
// as disk; disk exists for users tracing dataflows over an easily-inspectable
// medium). These prove the helper respects that order, including the debug flow
// where the client masks cuda+http from its own reachable set to force disk.
const SelectionRow kSoftRows[] = {
    {"soft-prefers-cuda-when-all-usable", "Soft preference cuda>http>disk, all usable — cuda chosen (top of order)",
     "The Notch client's canonical preference order is cuda > http > disk. With every transport usable, the helper picks the top of the order: cuda.",
     "cuda,disk,http", "cuda,disk,http", "cuda,disk,http", "cuda,http,disk", true, "cuda"},
    {"soft-prefers-http-over-disk", "Soft preference cuda>http>disk, no cuda — http chosen over disk",
     "With cuda not usable (no server GPU), the canonical order picks http over disk. This proves negotiation respects the real preference (http preferred to disk; disk is the debug/traceable transport), not an alphabetical default.",
     "cuda,disk,http", "disk,http", "disk,http", "cuda,http,disk", true, "http"},
    {"soft-debug-mask-forces-disk", "Debug mode: client masks cuda+http from its capabilities — disk chosen",
     "To trace a dataflow over an easily-inspectable transport, the client masks cuda and http from its own reachable set, leaving disk the only usable transport. Confirms a client can narrow its capabilities to force a transport.",
     "cuda,disk,http", "cuda,disk,http", "disk", "cuda,http,disk", true, "disk"},
    {"soft-fallback-when-no-order-match", "Soft preference lists only unusable transports — fall back to first usable",
     "Branch (c): no element of the preference order is usable, so the helper falls back to the first usable transport. Order is cuda only; usable is disk,http (non-image, no GPU on the client side) — cuda is not usable, so the fallback returns the first usable transport (disk).",
     "disk,http", "disk,http", "disk,http", "cuda", true, "disk"},
};

const char* const kSpecHard = "notch_conformance_spec.md §4 hard selection (A1 n A2 n A3, hard request)";
const char* const kSpecSoft = "notch_conformance_spec.md §4 soft selection (A4 preference order)";
const char* const kSpecTypeAxis = "notch_conformance_spec.md §4 type-axis discovery (A1)";
const char* const kSpecReadiness = "notch_conformance_spec.md §4 deployment-readiness decision (A5)";
const char* const kSpecLiveness = "notch_conformance_spec.md §4 setup/liveness (A0)";
const char* const kSpecDeliveryLocal = "notch_conformance_spec.md §4 Layer 2 delivery-local";
const char* const kSpecDeliveryRemote = "notch_conformance_spec.md §4 Layer 2 delivery-remote";
const char* const kSpecFacade = "notch_conformance_spec.md §4 facade behavior (caching/validation/correlation)";

} // namespace

// Records one case result, stamps its index, updates the running totals, and
// writes the self-describing evidence.
class CaseRecorder
{
public:
    CaseRecorder(CaseLogger& logger, MatrixSummary& summary)
        : m_logger(logger), m_summary(summary)
    {
    }

    // Records the case and returns its assigned index, so callers can attach
    // per-case evidence files (diagnostics, websocket frames, server.log slice).
    int Record(CaseRecord record)
    {
        record.index = ++m_index;
        if (record.result == "pass") ++m_summary.passed;
        else if (record.result == "fail") ++m_summary.failed;
        else if (record.result == "skip") ++m_summary.skipped;
        else ++m_summary.errored;
        m_logger.WriteCase(record);
        return record.index;
    }

private:
    CaseLogger& m_logger;
    MatrixSummary& m_summary;
    int m_index = 0;
};

namespace
{

// Build a synthetic OutputTransportRequest from explicit transport sets so the
// facade's SelectOutputTransport drives the pure (no-server) selection matrix and
// the reject decisions. type-allowed rides on the WorkflowOutput.transports and
// server-available rides on ServerFacts.output_transports — the same two
// Comfy-owned sets the facade reads from a parsed contract and negotiated session.
cec::OutputTransportRequest MakeTransportRequest(
    const std::vector<std::string>& typeAllowed,
    const std::vector<std::string>& serverAvailable,
    const std::vector<std::string>& clientReachable,
    const std::vector<std::string>& preferenceOrder,
    const std::string& requiredTransport)
{
    cec::OutputTransportRequest request;
    request.output.name = "selection";
    request.output.type = "synthetic";
    request.output.transports = typeAllowed;
    request.server.output_transports = serverAvailable;
    request.client_reachable_transports = clientReachable;
    request.preference_order = preferenceOrder;
    request.required_transport = requiredTransport;
    return request;
}

void RunSelectionRow(CaseRecorder& recorder, cec::Client& client, FacadeLog& facadeLog, const SelectionRow& row, bool soft)
{
    const std::vector<std::string> typeAllowed = Split(row.typeAllowed);
    const std::vector<std::string> serverAvailable = Split(row.serverAvailable);
    const std::vector<std::string> clientReachable = Split(row.clientReachable);
    const std::vector<std::string> preference = soft ? Split(row.requiredOrPreference) : std::vector<std::string>{};
    const std::string required = soft ? "" : row.requiredOrPreference;

    cec::OutputTransportRequest request =
        MakeTransportRequest(typeAllowed, serverAvailable, clientReachable, preference, required);
    cec::OutputTransportDecision decision = client.SelectOutputTransport(request);

    facadeLog.SetCase(row.id);
    facadeLog.Action("select_output_transport",
                     "\"transport\":" + CaseLogger::Quote(decision.transport),
                     decision.ok ? "ok" : "fail", decision.error);

    bool ok = decision.ok == row.expectOk && (!row.expectOk || decision.transport == row.expectTransport);

    CaseRecord rec;
    rec.caseId = row.id;
    rec.title = row.title;
    rec.phase = "transport-selection";
    rec.description = row.description;
    rec.specRef = soft ? kSpecSoft : kSpecHard;
    if (soft)
    {
        rec.preferredOrder = preference;
    }
    else
    {
        rec.requiredTransport = required;
    }
    std::ostringstream fixture;
    fixture << "{\"type_allowed\":" << CaseLogger::Array(typeAllowed)
            << ",\"server_available\":" << CaseLogger::Array(serverAvailable)
            << ",\"client_reachable\":" << CaseLogger::Array(clientReachable) << "}";
    rec.fixtureJson = fixture.str();
    rec.expectedJson = ExpectJson(row.expectOk, row.expectTransport);
    rec.actualJson = DecisionJson(decision);
    rec.result = ok ? "pass" : "fail";
    if (!ok)
    {
        rec.errors.push_back("transport_selection_mismatch");
    }
    recorder.Record(rec);
}

// Deployment-readiness decision (Layer 1, no execution): the facade's
// GetRequiredFiles posts /notch/get-required-files; any required file with
// exists=false ⇒ not-ready ⇒ the client would block the run. Verifies the
// readiness decision only — server-published facts, a user-actionable decision,
// orthogonal to transport selection.
void RunReadinessCase(
    CaseRecorder& recorder,
    cec::Client& client,
    FacadeLog& facadeLog,
    const std::string& caseId,
    const std::string& title,
    const std::string& description,
    const std::string& workflowJson,
    bool expectReady)
{
    CaseRecord rec;
    rec.caseId = caseId;
    rec.title = title;
    rec.phase = "readiness";
    rec.description = description;
    rec.specRef = kSpecReadiness;

    facadeLog.SetCase(caseId);
    cec::Result<std::vector<cec::RequiredFile> > files = client.GetRequiredFiles(workflowJson);
    if (!files)
    {
        facadeLog.Action("get_required_files", "", "fail", files.GetError().message);
        rec.result = "error";
        rec.actualJson = "{\"error\":" + CaseLogger::Quote(files.GetError().message) + "}";
        rec.errors.push_back("required_files_error");
        recorder.Record(rec);
        return;
    }
    int missing = 0;
    for (size_t i = 0; i < files.Value().size(); ++i)
    {
        if (!files.Value()[i].exists)
        {
            ++missing;
        }
    }
    facadeLog.Action("get_required_files", "\"missing\":" + std::to_string(missing), "ok");
    const bool ready = missing == 0;
    const bool ok = ready == expectReady;
    std::ostringstream actual;
    actual << "{\"ready\":" << CaseLogger::Bool(ready)
           << ",\"missing_files\":" << missing
           << ",\"client_would_block\":" << CaseLogger::Bool(!ready) << "}";
    rec.expectedJson = "{\"ready\":" + std::string(expectReady ? "true" : "false") + "}";
    rec.actualJson = actual.str();
    rec.result = ok ? "pass" : "fail";
    if (!ok)
    {
        rec.errors.push_back("deployment_readiness_mismatch");
    }
    recorder.Record(rec);
}

long FileSize(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        return -1;
    }
    return static_cast<long>(f.tellg());
}

std::string ReadFileSlice(const std::string& path, long start, long end)
{
    std::ifstream f(path, std::ios::binary);
    if (!f || start < 0 || end <= start)
    {
        return "";
    }
    f.seekg(start);
    std::string buffer(static_cast<size_t>(end - start), '\0');
    f.read(&buffer[0], end - start);
    buffer.resize(static_cast<size_t>(f.gcount()));
    return buffer;
}

bool ContainsString(const std::vector<std::string>& values, const std::string& target)
{
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (values[i] == target)
        {
            return true;
        }
    }
    return false;
}

bool IsSafeRelativePath(const std::string& path)
{
    if (path.empty() || path[0] == '/' || path[0] == '\\')
    {
        return false;
    }
    if (path.size() >= 2U && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
    {
        return false;
    }
    std::string segment;
    for (size_t i = 0; i <= path.size(); ++i)
    {
        const char c = i < path.size() ? path[i] : '/';
        if (c == '/' || c == '\\')
        {
            if (segment == "..")
            {
                return false;
            }
            segment.clear();
        }
        else
        {
            segment.push_back(c);
        }
    }
    return true;
}

std::string JoinPath(const std::string& root, const std::string& relativePath)
{
    if (root.empty())
    {
        return relativePath;
    }
    if (relativePath.empty())
    {
        return root;
    }
    if (root[root.size() - 1U] == '/' || root[root.size() - 1U] == '\\')
    {
        return root + relativePath;
    }
    return root + "/" + relativePath;
}

std::string NamedRouteRelativeDirectory(const MatrixOptions& options)
{
    if (!options.namedRouteRelativeDirectory.empty())
    {
        return options.namedRouteRelativeDirectory;
    }
    return "remote-route";
}

bool NamedRouteConfiguredForClient(const MatrixOptions& options, const cec::ServerFacts& server)
{
    return !options.namedRouteId.empty() && !options.namedRouteClientRoot.empty() &&
           ContainsString(server.named_disk_route_ids, options.namedRouteId);
}

// Current WSL2 GPU-PV runners can reject an otherwise well-formed CUDA IPC
// memory handle with InvalidResourceHandle. Treat that as a platform skip only
// after the server-side share contract has already passed; the separate
// cuda-ipc-probe artifact records whether raw IPC is actually supported on the
// runner. Native Linux import failures remain real failures.
bool IsWslPlatform()
{
    std::ifstream version("/proc/version");
    if (!version)
    {
        return false;
    }
    std::string line;
    std::getline(version, line);
    return line.find("microsoft") != std::string::npos || line.find("Microsoft") != std::string::npos ||
           line.find("WSL") != std::string::npos;
}

std::vector<std::string> DeliveryServerTransports(const cec::ServerFacts& server)
{
    std::vector<std::string> transports;
    for (size_t i = 0; i < server.output_transports.size(); ++i)
    {
        const std::string transport = server.output_transports[i];
        if (transport == "disk" || transport == "http" || transport == "cuda")
        {
            transports.push_back(transport);
        }
    }
    std::sort(transports.begin(), transports.end());
    transports.erase(std::unique(transports.begin(), transports.end()), transports.end());
    return transports;
}

std::vector<std::string> ClientReachableTransports(Phase phase)
{
    if (phase == Phase::DeliveryRemote)
    {
        return std::vector<std::string>{"http"};
    }
    return std::vector<std::string>{"cuda", "disk", "http"};
}

std::vector<std::string> ClientReachableTransports(
    Phase phase,
    bool namedRouteDisk,
    const MatrixOptions& options,
    const cec::ServerFacts& server)
{
    if (phase != Phase::DeliveryRemote)
    {
        return std::vector<std::string>{"cuda", "disk", "http"};
    }
    if (namedRouteDisk && NamedRouteConfiguredForClient(options, server))
    {
        return std::vector<std::string>{"disk", "http"};
    }
    return std::vector<std::string>{"http"};
}

std::string ExtensionFromPath(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    {
        return "bin";
    }
    return path.substr(dot + 1U);
}

std::string DefaultSourceFilePath(const MatrixOptions& options)
{
    if (!options.sourceFilePath.empty())
    {
        return options.sourceFilePath;
    }
    if (!options.assetRoot.empty())
    {
        return options.assetRoot + "/mesh.glb";
    }
    return "";
}

std::string DefaultLocalOutputPath(const MatrixOptions& options)
{
    if (!options.localOutputPath.empty())
    {
        return options.localOutputPath;
    }
    return "/tmp/notch-conformance-output";
}

std::string ServerLogPath(const MatrixOptions& options)
{
    if (!options.serverLogPath.empty())
    {
        return options.serverLogPath;
    }
    return options.outputRoot + "/comfyui.log";
}

std::string BuildRoundTripWorkflowJson(const std::string& inputType, const std::string& slotName)
{
    std::ostringstream json;
    json << "{"
         << "\"1\":{\"class_type\":\"NotchSingleInput\",\"inputs\":{"
         << "\"key\":\"test_input\",\"type\":" << CaseLogger::Quote(inputType)
         << ",\"frontend_state\":\"\"},\"_meta\":{\"title\":\"Notch Value Loader\"}},"
         << "\"2\":{\"class_type\":\"NotchOutputNode\",\"inputs\":{"
         << CaseLogger::Quote(slotName) << ":[\"1\",0]"
         << "},\"_meta\":{\"title\":\"Notch Output\"}}"
         << "}";
    return json.str();
}

std::vector<uint8_t> BuildFloat32RgbPattern(int width, int height)
{
    std::vector<uint8_t> bytes;
    bytes.resize(static_cast<size_t>(width * height * 3 * 4));
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float values[3] = {
                static_cast<float>(x) / static_cast<float>(width - 1),
                static_cast<float>(y) / static_cast<float>(height - 1),
                static_cast<float>((x + y) % width) / static_cast<float>(width - 1),
            };
            for (int c = 0; c < 3; ++c)
            {
                const size_t offset = static_cast<size_t>(((y * width + x) * 3 + c) * 4);
                const uint8_t* src = reinterpret_cast<const uint8_t*>(&values[c]);
                bytes[offset + 0U] = src[0];
                bytes[offset + 1U] = src[1];
                bytes[offset + 2U] = src[2];
                bytes[offset + 3U] = src[3];
            }
        }
    }
    return bytes;
}

std::vector<uint8_t> BuildExpectedFloat32RgbaPattern(int width, int height)
{
    std::vector<uint8_t> bytes;
    bytes.resize(static_cast<size_t>(width * height * 4 * 4));
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float values[4] = {
                static_cast<float>(x) / static_cast<float>(width - 1),
                static_cast<float>(y) / static_cast<float>(height - 1),
                static_cast<float>((x + y) % width) / static_cast<float>(width - 1),
                1.0f,
            };
            for (int c = 0; c < 4; ++c)
            {
                const size_t offset = static_cast<size_t>(((y * width + x) * 4 + c) * 4);
                const uint8_t* src = reinterpret_cast<const uint8_t*>(&values[c]);
                bytes[offset + 0U] = src[0];
                bytes[offset + 1U] = src[1];
                bytes[offset + 2U] = src[2];
                bytes[offset + 3U] = src[3];
            }
        }
    }
    return bytes;
}

struct DeliveryContext
{
    CaseRecorder& recorder;
    cec::Client& client;
    cec::HttpTransport& http;
    IWebSocketProbe& ws;
    CaseLogger& logger;
    FacadeLog& facadeLog;
    DeliveryEventSink& sink;
    DeliveryRunState& state;
    const MatrixOptions& options;
    const cec::ServerFacts& server;
    ICudaShareReader* cudaReader;
};

// Drain the live WebSocket and forward every frame to the facade
// (Client::OnWebSocketText). The facade parses, correlates prompt -> consumer,
// and dispatches to the DeliveryEventSink, which records the terminal lifecycle
// event, the matching notch-output-ready, and the matching notch-cuda-share-status
// into state. The harness owns this loop's timing because the facade is threadless.
void WaitForDelivery(DeliveryContext& ctx, const std::string& transport, int timeoutMs)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::vector<std::string> batch = ctx.ws.DrainReceived();
        for (size_t i = 0; i < batch.size(); ++i)
        {
            ctx.state.websocketFrames.push_back(batch[i]);
            ctx.client.OnWebSocketText(batch[i]);
        }
        if (ctx.state.terminalSuccess && (transport == "cuda" ? ctx.state.cudaStatus : ctx.state.outputReady))
        {
            return;
        }
        if (!ctx.state.terminalType.empty() && !ctx.state.terminalSuccess)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::string WebSocketFrameLog(const std::vector<std::string>& frames)
{
    std::string log;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        log += frames[i];
        log += "\n";
    }
    return log;
}

std::string OutputReadyJson(const cec::OutputReady& output)
{
    std::ostringstream json;
    json << "{\"name\":" << CaseLogger::Quote(output.name)
         << ",\"consumer_id\":" << CaseLogger::Quote(output.consumer_id)
         << ",\"prompt_id\":" << CaseLogger::Quote(output.prompt_id)
         << ",\"transport\":" << CaseLogger::Quote(output.transport)
         << ",\"path\":" << CaseLogger::Quote(output.path)
         << ",\"named_route_id\":" << CaseLogger::Quote(output.named_route_id)
         << ",\"relative_path\":" << CaseLogger::Quote(output.relative_path)
         << ",\"url\":" << CaseLogger::Quote(output.url)
         << ",\"type\":" << CaseLogger::Quote(output.type)
         << ",\"format\":" << CaseLogger::Quote(output.format)
         << ",\"width\":" << output.width
         << ",\"height\":" << output.height << "}";
    return json.str();
}

std::string CudaShareStatusJson(const cec::CudaShareStatus& share)
{
    std::ostringstream json;
    json << "{\"name\":" << CaseLogger::Quote(share.name)
         << ",\"width\":" << share.width
         << ",\"height\":" << share.height
         << ",\"channels\":" << share.channels
         << ",\"pixel_format\":" << CaseLogger::Quote(share.pixel_format)
         << ",\"dtype\":" << CaseLogger::Quote(share.dtype)
         << ",\"channel_order\":" << CaseLogger::Quote(share.channel_order)
         << ",\"memory_layout\":" << CaseLogger::Quote(share.memory_layout)
         << ",\"row_stride_bytes\":" << share.row_stride_bytes
         << ",\"data_offset\":" << share.data_offset
         << ",\"data_size_bytes\":" << share.data_size_bytes
         << ",\"metadata_offset\":" << share.metadata_offset
         << ",\"metadata_size_bytes\":" << share.metadata_size_bytes
         << ",\"frame_counter_offset\":" << share.frame_counter_offset
         << ",\"cuda_device_index\":" << share.cuda_device_index
         << ",\"notch_consumer_id\":" << CaseLogger::Quote(share.notch_consumer_id)
         << ",\"resource_id\":" << CaseLogger::Quote(share.resource_id)
         << ",\"has_ipc_handle\":" << CaseLogger::Bool(!share.ipc_handle_hex.empty())
         << ",\"has_event_ipc_handle\":" << CaseLogger::Bool(!share.event_ipc_handle_hex.empty())
         << "}";
    return json.str();
}

std::string WebSocketSummaryJson(const DeliveryRunState& state)
{
    std::ostringstream json;
    json << "{\"prompt_id\":" << CaseLogger::Quote(state.promptId)
         << ",\"queued\":" << CaseLogger::Bool(state.queued)
         << ",\"terminal_type\":" << CaseLogger::Quote(state.terminalType)
         << ",\"terminal_success\":" << CaseLogger::Bool(state.terminalSuccess)
         << ",\"output_ready\":" << CaseLogger::Bool(state.outputReady)
         << ",\"cuda_status\":" << CaseLogger::Bool(state.cudaStatus)
         << ",\"frame_count\":" << state.websocketFrames.size();
    if (state.outputReady)
    {
        json << ",\"output\":" << OutputReadyJson(state.output);
    }
    if (state.cudaStatus)
    {
        json << ",\"cuda_share\":" << CudaShareStatusJson(state.cudaShare);
    }
    json << "}";
    return json.str();
}

// Diagnostics are not part of the facade contract; fetch them with a direct GET
// for evidence/context only (spec §9b: diagnostics never gate a verdict).
bool FetchPromptDiagnostics(cec::HttpTransport& http, const std::string& promptId, std::string& body, std::string& error)
{
    body.clear();
    error.clear();
    if (promptId.empty())
    {
        error = "prompt_id is empty";
        return false;
    }
    cec::HttpRequest diag;
    diag.method = "GET";
    diag.path = "/notch/diagnostics?prompt_id=" + promptId;
    diag.content_type = "application/json";
    cec::HttpResponse response;
    std::string sendError;
    if (!http.Send(diag, response, sendError))
    {
        error = sendError.empty() ? "diagnostics request failed" : sendError;
        return false;
    }
    body = response.body;
    if (response.status_code != 200)
    {
        error = "diagnostics endpoint returned HTTP " + std::to_string(response.status_code);
        return false;
    }
    return true;
}

void AppendPromptDiagnostics(int index, const std::string& caseId, cec::HttpTransport& http, CaseLogger& logger,
                             const std::string& promptId)
{
    std::string body;
    std::string error;
    if (FetchPromptDiagnostics(http, promptId, body, error) && !body.empty())
    {
        logger.AppendCaseFile(index, caseId, "notch-diagnostics.json", CaseLogger::PrettyPrint(body) + "\n");
    }
}

std::vector<std::string> MissingDiagnosticsEvents(const std::string& diagnosticsJson,
                                                  const std::vector<std::string>& requiredEvents)
{
    std::vector<std::string> missing;
    for (size_t i = 0; i < requiredEvents.size(); ++i)
    {
        if (diagnosticsJson.find(requiredEvents[i]) == std::string::npos)
        {
            missing.push_back(requiredEvents[i]);
        }
    }
    return missing;
}

void AppendDeliveryEvidence(
    int index,
    const std::string& caseId,
    cec::HttpTransport& http,
    CaseLogger& logger,
    const MatrixOptions& options,
    const FacadeLog& facadeLog,
    const long logStart,
    const DeliveryRunState& state,
    const std::string& negotiationJson,
    const std::string& injectRequestBody,
    const std::string& injectResponseJson,
    const std::string& hashesJson,
    const std::string& diagnosticsJson = "")
{
    logger.AppendCaseFile(index, caseId, "negotiation.json", CaseLogger::PrettyPrint(negotiationJson) + "\n");
    if (!facadeLog.Lines().empty())
    {
        logger.AppendCaseFile(index, caseId, "facade-trace.log", facadeLog.Lines());
    }
    if (!injectRequestBody.empty())
    {
        // The facade builds the inject request; multipart bodies carry binary form
        // data, so summarize those and pretty-print JSON bodies.
        const bool looksJson = injectRequestBody[0] == '{' || injectRequestBody[0] == '[';
        if (looksJson)
        {
            logger.AppendCaseFile(index, caseId, "inject-request.json",
                                  CaseLogger::PrettyPrint(injectRequestBody) + "\n");
        }
        else
        {
            std::ostringstream note;
            note << "{\"note\":\"multipart/form-data body omitted (binary)\",\"bytes\":"
                 << injectRequestBody.size() << "}";
            logger.AppendCaseFile(index, caseId, "inject-request.json", CaseLogger::PrettyPrint(note.str()) + "\n");
        }
    }
    if (!injectResponseJson.empty())
    {
        logger.AppendCaseFile(index, caseId, "inject-response.json", CaseLogger::PrettyPrint(injectResponseJson) + "\n");
    }
    if (!state.websocketFrames.empty())
    {
        logger.AppendCaseFile(index, caseId, "websocket.jsonl", WebSocketFrameLog(state.websocketFrames));
        logger.AppendCaseFile(index, caseId, "websocket-summary.json",
                              CaseLogger::PrettyPrint(WebSocketSummaryJson(state)) + "\n");
    }
    if (state.outputReady)
    {
        logger.AppendCaseFile(index, caseId, "output-ready.json",
                              CaseLogger::PrettyPrint(OutputReadyJson(state.output)) + "\n");
    }
    if (state.cudaStatus)
    {
        logger.AppendCaseFile(index, caseId, "cuda-share-status.json",
                              CaseLogger::PrettyPrint(CudaShareStatusJson(state.cudaShare)) + "\n");
    }
    if (!hashesJson.empty())
    {
        logger.AppendCaseFile(index, caseId, "hashes.json", CaseLogger::PrettyPrint(hashesJson) + "\n");
    }
    const std::string serverLog = ServerLogPath(options);
    const long logEnd = FileSize(serverLog);
    const std::string slice = ReadFileSlice(serverLog, logStart, logEnd);
    if (!slice.empty())
    {
        logger.AppendCaseFile(index, caseId, "server.log", slice);
    }
    if (!diagnosticsJson.empty())
    {
        logger.AppendCaseFile(index, caseId, "notch-diagnostics.json",
                              CaseLogger::PrettyPrint(diagnosticsJson) + "\n");
    }
    else
    {
        AppendPromptDiagnostics(index, caseId, http, logger, state.promptId);
    }
}

// The server's output routes guard the consumer/output id with ^[A-Za-z0-9_-]+$,
// so derive a safe consumer id (dots and any other disallowed char -> '-') for
// output routing while the dotted case id stays the human-facing record id.
std::string SafeConsumerId(const std::string& caseId)
{
    std::string out;
    out.reserve(caseId.size());
    for (char c : caseId)
    {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                          c == '-';
        out.push_back(safe ? c : '-');
    }
    return out;
}

std::string OutputExtensionForType(const std::string& outputType, const std::string& sourcePath)
{
    if (outputType == "file_path") return ExtensionFromPath(sourcePath);
    if (outputType == "file_3d") return "glb";
    if (outputType == "mesh") return "glb";
    if (outputType == "image") return "png";
    if (outputType == "audio") return "wav";
    if (outputType == "video") return "mp4";
    if (outputType == "load3d_camera") return "json";
    return "bin";
}

// A default load3d_camera value (inline dict merged into the server's defaults).
const char* const kCameraInlineJson = "{\"position\":[0.0,0.0,5.0],\"target\":[0.0,0.0,0.0],\"fov\":35.0}";

// Build the facade OutputRequest for a transport, output type, and extension.
cec::OutputRequest BuildOutputRequest(const std::string& transport, const std::string& outputType,
                                      const std::string& extension, const MatrixOptions& options,
                                      bool useNamedRouteDisk, const std::string& caseId)
{
    if (transport == "http")
    {
        return cec::OutputRequest::Http(outputType, extension);
    }
    if (transport == "cuda")
    {
        return cec::OutputRequest::Cuda(outputType);
    }
    // disk
    if (useNamedRouteDisk)
    {
        return cec::OutputRequest::DiskNamedRoute(outputType, options.namedRouteId, extension,
                                                  NamedRouteRelativeDirectory(options), caseId);
    }
    return cec::OutputRequest::DiskPath(outputType, DefaultLocalOutputPath(options), extension, caseId);
}

// Run a verifier for the type's verification class against the delivered bytes.
// expectedBytes is the uploaded/source bytes (used by byte-exact only).
VerifyResult RunVerifier(const DeliveryTypeContract& contract,
                         const std::vector<uint8_t>& expectedBytes,
                         const std::vector<uint8_t>& deliveredBytes)
{
    switch (contract.verify)
    {
    case VerificationClass::ByteExact:
        return VerifyByteExact(expectedBytes, deliveredBytes);
    case VerificationClass::Structural:
        return VerifyStructural(deliveredBytes);
    case VerificationClass::DecodedImage:
        // stb_image is not vendored yet; until then verify the delivered image is a
        // real, non-empty artifact (integrity). Upgrades to dims+sample-hash decode
        // without changing the case set when the decoder lands.
        return VerifyIntegrity(deliveredBytes);
    case VerificationClass::Integrity:
    case VerificationClass::CudaRaw:
    default:
        return VerifyIntegrity(deliveredBytes);
    }
}

void RunFilePathDeliveryCase(
    DeliveryContext& ctx,
    const std::string& caseId,
    const std::string& title,
    const std::string& description,
    const std::string& transport,
    bool useNamedRouteDisk,
    const DeliveryTypeContract* contractPtr)
{
    static const DeliveryTypeContract kFilePathContract{
        "file_path", "STRING", "file_path", "", false, VerificationClass::ByteExact};
    const DeliveryTypeContract& contract = contractPtr ? *contractPtr : kFilePathContract;
    const MatrixOptions& options = ctx.options;

    CaseRecord rec;
    rec.caseId = caseId;
    rec.title = title;
    rec.phase = options.phase == Phase::DeliveryRemote ? "delivery-remote" : "delivery-local";
    rec.description = description;
    rec.specRef = options.phase == Phase::DeliveryRemote ? kSpecDeliveryRemote : kSpecDeliveryLocal;
    rec.requiredTransport = transport;

    ctx.facadeLog.SetCase(caseId);
    ctx.state = DeliveryRunState{};

    const std::vector<std::string> clientReachable =
        ClientReachableTransports(options.phase, useNamedRouteDisk, options, ctx.server);
    const std::vector<std::string> typeAllowed = TypeAllowedTransports(contract);
    const std::vector<std::string> serverAvailable = DeliveryServerTransports(ctx.server);
    cec::OutputTransportRequest selectionRequest =
        MakeTransportRequest(typeAllowed, serverAvailable, clientReachable, std::vector<std::string>{}, transport);
    cec::OutputTransportDecision decision = ctx.client.SelectOutputTransport(selectionRequest);
    ctx.facadeLog.Action("select_output_transport", "\"transport\":" + CaseLogger::Quote(decision.transport),
                         decision.ok ? "ok" : "fail", decision.error);

    std::ostringstream negotiation;
    negotiation << "{\"type_allowed\":" << CaseLogger::Array(typeAllowed)
                << ",\"server_available\":" << CaseLogger::Array(serverAvailable)
                << ",\"client_reachable\":" << CaseLogger::Array(clientReachable)
                << ",\"server_named_route_ids\":" << CaseLogger::Array(ctx.server.named_disk_route_ids)
                << ",\"named_route_id\":" << CaseLogger::Quote(useNamedRouteDisk ? options.namedRouteId : "")
                << ",\"required_transport\":" << CaseLogger::Quote(transport)
                << ",\"choice\":" << DecisionJson(decision) << "}";

    // load3d_camera is injected as inline JSON; every other type has a source
    // file: file_path uses it as a server-staged path, the rest upload its bytes.
    const bool inlineInput = contract.outputType == "load3d_camera";
    std::string sourcePath;
    std::vector<uint8_t> sourceBytes;
    if (!inlineInput)
    {
        sourcePath = !contract.fixtureFile.empty() ? JoinPath(options.assetRoot, contract.fixtureFile)
                                                   : DefaultSourceFilePath(options);
        std::string fileError;
        if (sourcePath.empty() || !ReadBinaryFile(sourcePath, sourceBytes, fileError))
        {
            rec.expectedJson = "{\"fixture\":\"source_file_readable\"}";
            rec.actualJson = "{\"source_path\":" + CaseLogger::Quote(sourcePath) +
                             ",\"error\":" + CaseLogger::Quote(fileError) + "}";
            rec.result = "error";
            rec.errors.push_back("harness_error");
            ctx.recorder.Record(rec);
            return;
        }
    }

    if (!decision.ok)
    {
        rec.expectedJson = "{\"selected\":true,\"terminal\":\"execution_success\",\"hash_match\":true}";
        rec.actualJson = "{\"selected\":false,\"choice\":" + DecisionJson(decision) + "}";
        rec.result = "fail";
        rec.errors.push_back("unexpected_reject");
        const int index = ctx.recorder.Record(rec);
        AppendDeliveryEvidence(index, caseId, ctx.http, ctx.logger, options, ctx.facadeLog, -1, ctx.state,
                               negotiation.str(), "", "", "");
        return;
    }

    const std::string consumerId = SafeConsumerId(caseId);
    const std::string workflowJson = BuildRoundTripWorkflowJson(contract.inputType, contract.slotName);

    // Parse the workflow through the facade first: this caches the contract that
    // Submit's pre-flight validation and the stateful SelectOutputTransport rely on.
    cec::Result<cec::WorkflowContract> parsed = ctx.client.ParseWorkflow(workflowJson);
    ctx.facadeLog.Action("parse_workflow",
                         parsed ? ("\"inputs\":" + std::to_string(parsed.Value().inputs.size()) +
                                   ",\"outputs\":" + std::to_string(parsed.Value().outputs.size()))
                                : "",
                         parsed ? "ok" : "fail", parsed ? "" : parsed.GetError().message);

    cec::SubmitRequest submit;
    submit.workflow_json = workflowJson;
    submit.client_id = options.clientId;
    submit.consumer_id = consumerId;
    submit.execute = true;
    submit.broadcast_ws = true;
    if (contract.outputType == "file_path")
    {
        submit.AddInput(cec::InputValue::String("test_input", sourcePath, contract.inputType));
    }
    else if (inlineInput)
    {
        submit.AddInput(cec::InputValue::Json("test_input", kCameraInlineJson, contract.inputType));
    }
    else
    {
        const std::string uploadName = contract.fixtureFile.empty() ? "input.bin" : contract.fixtureFile;
        submit.AddInput(cec::InputValue::Binary("test_input", sourceBytes, uploadName, contract.inputType));
    }
    submit.output = BuildOutputRequest(transport, contract.outputType,
                                       OutputExtensionForType(contract.outputType, sourcePath), options,
                                       useNamedRouteDisk, caseId);

    ctx.sink.Configure("", consumerId, transport);

    const long logStart = FileSize(ServerLogPath(options));
    cec::Result<cec::JobHandle> job = ctx.client.Submit(submit);
    // The facade builds and sends the inject request internally; the raw body is
    // captured by http.jsonl and the submit step in facade-trace.log.
    const std::string injectRequestBody;
    ctx.facadeLog.Action("submit",
                         job ? ("\"queued\":" + CaseLogger::Bool(job.Value().queued) + ",\"prompt_id\":" +
                                CaseLogger::Quote(job.Value().prompt_id))
                             : "",
                         job ? "ok" : "fail", job ? "" : job.GetError().message);

    if (!job)
    {
        ctx.state.injectError = job.GetError().message;
        rec.result = "error";
        rec.actualJson = "{\"submit_error\":" + CaseLogger::Quote(job.GetError().message) + "}";
        rec.errors.push_back("harness_error");
        const int index = ctx.recorder.Record(rec);
        AppendDeliveryEvidence(index, caseId, ctx.http, ctx.logger, options, ctx.facadeLog, logStart, ctx.state,
                               negotiation.str(), "", "", "");
        return;
    }

    ctx.state.queued = job.Value().queued;
    ctx.state.promptId = job.Value().prompt_id;
    ctx.sink.Configure(ctx.state.promptId, consumerId, transport);

    if (ctx.state.queued)
    {
        WaitForDelivery(ctx, transport, options.executeTimeoutMs);
    }

    std::vector<uint8_t> outputBytes;
    std::string outputPath;
    std::string outputError;
    bool outputRead = false;
    int outputHttpStatus = 0;
    if (ctx.state.outputReady)
    {
        if (transport == "disk")
        {
            if (useNamedRouteDisk)
            {
                if (ctx.state.output.named_route_id != options.namedRouteId)
                {
                    outputError = "named_route_id mismatch";
                }
                else if (!IsSafeRelativePath(ctx.state.output.relative_path))
                {
                    outputError = "unsafe or empty relative_path";
                }
                else
                {
                    outputPath = JoinPath(options.namedRouteClientRoot, ctx.state.output.relative_path);
                    outputRead = ReadBinaryFile(outputPath, outputBytes, outputError);
                }
            }
            else
            {
                outputPath = ctx.state.output.path;
                outputRead = ReadBinaryFile(outputPath, outputBytes, outputError);
            }
        }
        else if (transport == "http")
        {
            // Fetch the artifact through the facade (Client::FetchOutput GETs the url).
            cec::Result<cec::GeneratedResult> fetched = ctx.client.FetchOutput(ctx.state.output);
            ctx.facadeLog.Action("fetch_output",
                                 fetched ? ("\"status\":" + std::to_string(fetched.Value().status_code) +
                                            ",\"bytes\":" + std::to_string(fetched.Value().bytes.size()))
                                         : "",
                                 fetched ? "ok" : "fail", fetched ? "" : fetched.GetError().message);
            if (fetched)
            {
                outputHttpStatus = fetched.Value().status_code;
                outputBytes.assign(fetched.Value().bytes.begin(), fetched.Value().bytes.end());
                outputRead = true;
                outputPath = ctx.state.output.url;
            }
            else
            {
                outputHttpStatus = fetched.GetError().status_code;
                outputError = fetched.GetError().message.empty() ? "http output fetch failed"
                                                                 : fetched.GetError().message;
            }
        }
    }

    const std::string inputHash = sourceBytes.empty() ? "" : Sha256Bytes(sourceBytes);
    const std::string outputHash = outputRead ? Sha256Bytes(outputBytes) : "";
    const VerifyResult verify = outputRead ? RunVerifier(contract, sourceBytes, outputBytes) : VerifyResult{};
    const bool verifyOk = outputRead && verify.ok;
    const bool routeHandoffApplies = useNamedRouteDisk && transport == "disk";
    const bool namedRouteHandoffOk = !routeHandoffApplies ||
        (ctx.state.outputReady && ctx.state.output.named_route_id == options.namedRouteId &&
         IsSafeRelativePath(ctx.state.output.relative_path) && ctx.state.output.path.empty());
    // Facade correlation assertion: the facade resolves the owning consumer from
    // its prompt -> consumer cache, so a delivered OutputReady carries our
    // consumer_id even though the wire event keys on the output name.
    const bool correlationOk = !ctx.state.outputReady || ctx.state.output.consumer_id == consumerId;
    if (ctx.state.outputReady)
    {
        ctx.facadeLog.Action("assert.correlation",
                             "\"consumer_id\":" + CaseLogger::Quote(ctx.state.output.consumer_id),
                             correlationOk ? "ok" : "fail");
    }

    std::string diagnosticsJson;
    std::string diagnosticsError;
    const bool diagnosticsFetched = ctx.state.promptId.empty()
        ? false
        : FetchPromptDiagnostics(ctx.http, ctx.state.promptId, diagnosticsJson, diagnosticsError);
    std::vector<std::string> requiredDiagnosticsEvents;
    if (ctx.state.queued)
    {
        requiredDiagnosticsEvents = {
            "inject.request_parsed",
            "inject.output_config_patched",
            "workflow.inputs_resolve_start",
            "workflow.inputs_stamped",
            "inject.inputs_stamped",
            "inject.queue_submit",
            "inject.queue_response",
            "single_input.resolve_value",
            "single_input.parse_value",
            "output_node.config_resolved",
            "output_node.payload_resolved",
            "output_node.delivery_start",
            "output.disk_delivery_start",
            "output.disk_delivery_done",
            "output.ready_broadcast",
            "output_node.delivery_done",
        };
    }
    const std::vector<std::string> missingDiagnostics =
        diagnosticsFetched ? MissingDiagnosticsEvents(diagnosticsJson, requiredDiagnosticsEvents)
                           : requiredDiagnosticsEvents;
    const bool ok = ctx.state.queued && ctx.state.terminalSuccess && ctx.state.outputReady &&
                    namedRouteHandoffOk && correlationOk && verifyOk;
    if (!ctx.state.queued)
    {
        rec.errors.push_back("unexpected_reject");
    }
    if (ctx.state.queued && !ctx.state.terminalSuccess)
    {
        rec.errors.push_back(ctx.state.terminalType.empty() ? "websocket_timeout" : "execution_error");
    }
    if (ctx.state.terminalSuccess && !ctx.state.outputReady)
    {
        rec.errors.push_back("output_event_missing");
    }
    if (ctx.state.outputReady && !outputRead)
    {
        rec.errors.push_back("output_artifact_missing");
    }
    if (ctx.state.outputReady && !namedRouteHandoffOk)
    {
        rec.errors.push_back("named_route_handoff_mismatch");
    }
    if (ctx.state.outputReady && !correlationOk)
    {
        rec.errors.push_back("consumer_correlation_mismatch");
    }
    if (outputRead && !verify.ok && !verify.error.empty())
    {
        rec.errors.push_back(verify.error);
    }

    std::ostringstream hashes;
    hashes << "{\"inputs\":[{\"name\":\"test_input\",\"type\":" << CaseLogger::Quote(contract.inputType)
           << ",\"path\":" << CaseLogger::Quote(sourcePath) << ",\"bytes\":" << sourceBytes.size()
           << ",\"sha256\":" << CaseLogger::Quote(inputHash) << "}],"
           << "\"outputs\":[{\"name\":" << CaseLogger::Quote(consumerId)
           << ",\"transport\":" << CaseLogger::Quote(transport)
           << ",\"output_type\":" << CaseLogger::Quote(contract.outputType)
           << ",\"path\":" << CaseLogger::Quote(outputPath)
           << ",\"named_route_id\":" << CaseLogger::Quote(ctx.state.output.named_route_id)
           << ",\"relative_path\":" << CaseLogger::Quote(ctx.state.output.relative_path)
           << ",\"bytes\":" << outputBytes.size()
           << ",\"sha256\":" << CaseLogger::Quote(outputHash)
           << ",\"verification\":" << (verify.detail.empty() ? "null" : verify.detail) << "}]}";

    std::ostringstream actual;
    actual << "{\"selected_transport\":" << CaseLogger::Quote(decision.transport)
           << ",\"output_type\":" << CaseLogger::Quote(contract.outputType)
           << ",\"queued\":" << CaseLogger::Bool(ctx.state.queued)
           << ",\"prompt_id\":" << CaseLogger::Quote(ctx.state.promptId)
           << ",\"terminal\":" << CaseLogger::Quote(ctx.state.terminalType)
           << ",\"output_ready\":" << CaseLogger::Bool(ctx.state.outputReady)
           << ",\"output_read\":" << CaseLogger::Bool(outputRead)
           << ",\"output_consumer_id\":" << CaseLogger::Quote(ctx.state.output.consumer_id)
           << ",\"correlation_ok\":" << CaseLogger::Bool(correlationOk)
           << ",\"output_url\":" << CaseLogger::Quote(ctx.state.output.url)
           << ",\"output_http_status\":" << outputHttpStatus
           << ",\"named_route_handoff_ok\":" << CaseLogger::Bool(namedRouteHandoffOk)
           << ",\"verify_ok\":" << CaseLogger::Bool(verifyOk)
           << ",\"verification\":" << (verify.detail.empty() ? "null" : verify.detail)
           << ",\"diagnostics_fetched\":" << CaseLogger::Bool(diagnosticsFetched)
           << ",\"diagnostics_missing_events\":" << CaseLogger::Array(missingDiagnostics)
           << ",\"input_sha256\":" << CaseLogger::Quote(inputHash)
           << ",\"output_sha256\":" << CaseLogger::Quote(outputHash);
    if (!outputError.empty())
    {
        actual << ",\"output_error\":" << CaseLogger::Quote(outputError);
    }
    if (!diagnosticsError.empty())
    {
        actual << ",\"diagnostics_error\":" << CaseLogger::Quote(diagnosticsError);
    }
    actual << "}";
    std::ostringstream expected;
    expected << "{\"selected\":true,\"terminal\":\"execution_success\",\"output_ready\":true";
    if (routeHandoffApplies)
    {
        expected << ",\"named_route_handoff_ok\":true";
    }
    expected << ",\"verify_ok\":true}";
    rec.expectedJson = expected.str();
    rec.actualJson = actual.str();
    rec.result = ok ? "pass" : "fail";

    const int index = ctx.recorder.Record(rec);
    AppendDeliveryEvidence(index, caseId, ctx.http, ctx.logger, options, ctx.facadeLog, logStart, ctx.state,
                           negotiation.str(), injectRequestBody, "", hashes.str(), diagnosticsJson);
}

void RunCudaDeliveryCase(
    DeliveryContext& ctx,
    const std::string& caseId,
    const std::string& title,
    const std::string& description)
{
    const MatrixOptions& options = ctx.options;
    CaseRecord rec;
    rec.caseId = caseId;
    rec.title = title;
    rec.phase = options.phase == Phase::DeliveryRemote ? "delivery-remote" : "delivery-local";
    rec.description = description;
    rec.specRef = options.phase == Phase::DeliveryRemote ? kSpecDeliveryRemote : kSpecDeliveryLocal;
    rec.requiredTransport = "cuda";

    ctx.facadeLog.SetCase(caseId);
    ctx.state = DeliveryRunState{};

    const std::vector<std::string> clientReachable = ClientReachableTransports(options.phase);
    const std::vector<std::string> serverAvailable = DeliveryServerTransports(ctx.server);
    cec::OutputTransportRequest selectionRequest =
        MakeTransportRequest(std::vector<std::string>{"cuda", "disk", "http"}, serverAvailable, clientReachable,
                             std::vector<std::string>{}, "cuda");
    cec::OutputTransportDecision decision = ctx.client.SelectOutputTransport(selectionRequest);
    ctx.facadeLog.Action("select_output_transport", "\"transport\":" + CaseLogger::Quote(decision.transport),
                         decision.ok ? "ok" : "fail", decision.error);

    std::ostringstream negotiation;
    negotiation << "{\"type_allowed\":[\"cuda\",\"disk\",\"http\"]"
                << ",\"server_available\":" << CaseLogger::Array(serverAvailable)
                << ",\"client_reachable\":" << CaseLogger::Array(clientReachable)
                << ",\"required_transport\":\"cuda\""
                << ",\"choice\":" << DecisionJson(decision) << "}";

    if (!decision.ok || !ContainsString(serverAvailable, "cuda") || ctx.server.cuda_device_index < 0)
    {
        rec.expectedJson = "{\"cuda_available\":true}";
        rec.actualJson = "{\"cuda_available\":false,\"choice\":" + DecisionJson(decision) + "}";
        rec.result = "skip";
        rec.errors.push_back("cuda_unavailable");
        const int index = ctx.recorder.Record(rec);
        AppendDeliveryEvidence(index, caseId, ctx.http, ctx.logger, options, ctx.facadeLog, -1, ctx.state,
                               negotiation.str(), "", "", "");
        return;
    }

    if (ctx.cudaReader == nullptr)
    {
        rec.expectedJson = "{\"cuda_reader_available\":true}";
        rec.actualJson = "{\"cuda_reader_available\":false}";
        rec.result = "skip";
        rec.errors.push_back("cuda_reader_unavailable");
        ctx.recorder.Record(rec);
        return;
    }

    const int width = 8;
    const int height = 8;
    std::vector<uint8_t> imageBytes = BuildFloat32RgbPattern(width, height);
    std::vector<uint8_t> expectedCudaBytes = BuildExpectedFloat32RgbaPattern(width, height);

    const std::string consumerId = SafeConsumerId(caseId);
    const std::string workflowJson = BuildRoundTripWorkflowJson("IMAGE", "image");

    cec::Result<cec::WorkflowContract> parsed = ctx.client.ParseWorkflow(workflowJson);
    ctx.facadeLog.Action("parse_workflow",
                         parsed ? ("\"outputs\":" + std::to_string(parsed.Value().outputs.size())) : "",
                         parsed ? "ok" : "fail", parsed ? "" : parsed.GetError().message);

    cec::InputValue imageInput =
        cec::InputValue::Binary("test_input", imageBytes, "cuda_input.float32rgb", "IMAGE");
    imageInput.raw_buffer.enabled = true;
    imageInput.raw_buffer.width = width;
    imageInput.raw_buffer.height = height;
    imageInput.raw_buffer.format = "float32_rgb";
    imageInput.raw_buffer.stride = width * 3 * 4;

    cec::SubmitRequest submit;
    submit.workflow_json = workflowJson;
    submit.client_id = options.clientId;
    submit.consumer_id = consumerId;
    submit.execute = true;
    submit.broadcast_ws = true;
    submit.AddInput(imageInput);
    submit.output = cec::OutputRequest::Cuda("image");

    ctx.sink.Configure("", consumerId, "cuda");

    const long logStart = FileSize(ServerLogPath(options));
    cec::Result<cec::JobHandle> job = ctx.client.Submit(submit);
    ctx.facadeLog.Action("submit",
                         job ? ("\"queued\":" + CaseLogger::Bool(job.Value().queued) + ",\"prompt_id\":" +
                                CaseLogger::Quote(job.Value().prompt_id))
                             : "",
                         job ? "ok" : "fail", job ? "" : job.GetError().message);
    if (!job)
    {
        ctx.state.injectError = job.GetError().message;
        rec.result = "error";
        rec.actualJson = "{\"submit_error\":" + CaseLogger::Quote(job.GetError().message) + "}";
        rec.errors.push_back("harness_error");
        const int index = ctx.recorder.Record(rec);
        AppendDeliveryEvidence(index, caseId, ctx.http, ctx.logger, options, ctx.facadeLog, logStart, ctx.state,
                               negotiation.str(), "", "", "");
        return;
    }
    ctx.state.queued = job.Value().queued;
    ctx.state.promptId = job.Value().prompt_id;
    ctx.sink.Configure(ctx.state.promptId, consumerId, "cuda");
    if (ctx.state.queued)
    {
        WaitForDelivery(ctx, "cuda", options.executeTimeoutMs);
    }
    // Keep the WebSocket OPEN through the share-info GET and the IPC import: the
    // server releases a client's CUDA output allocation on /ws disconnect, so a
    // close here would free the device allocation and make both the info endpoint
    // 404 and the IPC handle stale. The harness owns one long-lived socket, so the
    // allocation stays alive until RunConformance closes it.

    cec::HttpRequest infoRequest;
    infoRequest.method = "GET";
    infoRequest.path = "/notch/cuda/share/" + consumerId;
    cec::HttpResponse infoResponse;
    std::string infoError;
    bool infoOk = ctx.http.Send(infoRequest, infoResponse, infoError) && infoResponse.status_code == 200;

    std::string cudaAvailableError;
    const bool cudaAvailable = ctx.cudaReader->CudaAvailable(cudaAvailableError);

    const std::string wsHandleHex = ctx.state.cudaShare.ipc_handle_hex;
    const std::string httpHandleHex = infoOk ? ExtractJsonStringField(infoResponse.body, "ipc_handle") : "";
    const bool handleWellFormed = wsHandleHex.size() == 128;  // 64-byte IPC handle
    const bool handleIntegrityOk = handleWellFormed && !httpHandleHex.empty() && httpHandleHex == wsHandleHex;

    const int wsDeviceIndex = ctx.state.cudaShare.cuda_device_index;
    const int httpDeviceIndex = infoOk ? ExtractJsonIntField(infoResponse.body, "cuda_device_index", -1) : -1;
    const int featuresDeviceIndex = ctx.server.cuda_device_index;
    const bool deviceIndexMatch =
        wsDeviceIndex >= 0 && wsDeviceIndex == httpDeviceIndex && wsDeviceIndex == featuresDeviceIndex;

    const std::string inputHash = Sha256Bytes(imageBytes);
    const std::string expectedOutputHash = Sha256Bytes(expectedCudaBytes);
    std::vector<uint8_t> cudaBytes;
    std::string cudaReadError;
    const bool cudaRead = ctx.state.cudaStatus && ctx.cudaReader->ReadShare(ctx.state.cudaShare, cudaBytes, cudaReadError);
    const std::string outputHash = cudaRead ? Sha256Bytes(cudaBytes) : "";
    const bool hashMatch = cudaRead && outputHash == expectedOutputHash;
    const bool shapeOk = ctx.state.cudaStatus && ctx.state.cudaShare.width == width &&
                         ctx.state.cudaShare.height == height && ctx.state.cudaShare.channels == 4 &&
                         ctx.state.cudaShare.dtype == "float32";
    std::string diagnosticsJson;
    std::string diagnosticsError;
    const bool diagnosticsFetched = ctx.state.promptId.empty()
        ? false
        : FetchPromptDiagnostics(ctx.http, ctx.state.promptId, diagnosticsJson, diagnosticsError);
    std::vector<std::string> requiredDiagnosticsEvents;
    if (ctx.state.queued)
    {
        requiredDiagnosticsEvents = {
            "inject.request_parsed",
            "input.multipart_processed",
            "inject.output_config_patched",
            "workflow.inputs_resolve_start",
            "workflow.inputs_stamped",
            "inject.inputs_stamped",
            "inject.queue_submit",
            "inject.queue_response",
            "single_input.resolve_value",
            "single_input.parse_value",
            "output_node.config_resolved",
            "output_node.payload_resolved",
            "output_node.delivery_start",
            "output.cuda_delivery_start",
            "cuda.share_status_broadcast",
            "output.cuda_delivery_done",
            "output_node.delivery_done",
        };
    }
    const std::vector<std::string> missingDiagnostics =
        diagnosticsFetched ? MissingDiagnosticsEvents(diagnosticsJson, requiredDiagnosticsEvents)
                           : requiredDiagnosticsEvents;
    const bool shareContractOk = ctx.state.queued && ctx.state.terminalSuccess && ctx.state.cudaStatus && shapeOk &&
                                 infoOk && handleIntegrityOk && deviceIndexMatch;
    std::string result;
    if (!cudaAvailable)
    {
        result = "skip";
        rec.errors.push_back("cuda_unavailable");
    }
    else if (shareContractOk && !cudaRead && IsWslPlatform())
    {
        result = "skip";
        rec.errors.push_back("cuda_ipc_unsupported");
    }
    else
    {
        if (!ctx.state.queued)
        {
            rec.errors.push_back("unexpected_reject");
        }
        if (ctx.state.queued && !ctx.state.terminalSuccess)
        {
            rec.errors.push_back(ctx.state.terminalType.empty() ? "websocket_timeout" : "execution_error");
        }
        if (ctx.state.terminalSuccess && !ctx.state.cudaStatus)
        {
            rec.errors.push_back("cuda_status_missing");
        }
        if (ctx.state.cudaStatus && !shapeOk)
        {
            rec.errors.push_back("cuda_metadata_mismatch");
        }
        if (ctx.state.cudaStatus && !infoOk)
        {
            rec.errors.push_back("cuda_info_unavailable");
        }
        if (ctx.state.cudaStatus && infoOk && !handleIntegrityOk)
        {
            rec.errors.push_back("cuda_handle_integrity_mismatch");
        }
        if (ctx.state.cudaStatus && infoOk && !deviceIndexMatch)
        {
            rec.errors.push_back("cuda_device_index_mismatch");
        }
        if (shareContractOk && !cudaRead)
        {
            rec.errors.push_back("cuda_import_failed");
        }
        if (cudaRead && !hashMatch)
        {
            rec.errors.push_back("output_hash_mismatch");
        }
        result = (shareContractOk && cudaRead && hashMatch) ? "pass" : "fail";
    }

    std::ostringstream hashes;
    hashes << "{\"inputs\":[{\"name\":\"test_input\",\"type\":\"IMAGE\",\"path\":"
           << CaseLogger::Quote("raw-buffer:float32_rgb") << ",\"bytes\":" << imageBytes.size()
           << ",\"sha256\":" << CaseLogger::Quote(inputHash) << "}],"
           << "\"outputs\":[{\"name\":" << CaseLogger::Quote(consumerId)
           << ",\"transport\":\"cuda\",\"comparison\":\"exact_float32_rgba_cuda_buffer\","
           << "\"bytes\":" << cudaBytes.size()
           << ",\"sha256\":" << CaseLogger::Quote(outputHash)
           << ",\"expected_sha256\":" << CaseLogger::Quote(expectedOutputHash)
           << ",\"hash_match\":" << CaseLogger::Bool(hashMatch)
           << ",\"cuda_read\":" << CaseLogger::Bool(cudaRead)
           << ",\"width\":" << ctx.state.cudaShare.width
           << ",\"height\":" << ctx.state.cudaShare.height
           << ",\"channels\":" << ctx.state.cudaShare.channels
           << ",\"dtype\":" << CaseLogger::Quote(ctx.state.cudaShare.dtype)
           << ",\"data_size_bytes\":" << ctx.state.cudaShare.data_size_bytes << "}]}";

    std::ostringstream actual;
    actual << "{\"selected_transport\":" << CaseLogger::Quote(decision.transport)
           << ",\"queued\":" << CaseLogger::Bool(ctx.state.queued)
           << ",\"prompt_id\":" << CaseLogger::Quote(ctx.state.promptId)
           << ",\"terminal\":" << CaseLogger::Quote(ctx.state.terminalType)
           << ",\"cuda_status\":" << CaseLogger::Bool(ctx.state.cudaStatus)
           << ",\"cuda_info_get\":" << CaseLogger::Bool(infoOk)
           << ",\"handle_integrity_ok\":" << CaseLogger::Bool(handleIntegrityOk)
           << ",\"ws_http_handle_match\":" << CaseLogger::Bool(!wsHandleHex.empty() && wsHandleHex == httpHandleHex)
           << ",\"device_index_match\":" << CaseLogger::Bool(deviceIndexMatch)
           << ",\"ws_device_index\":" << wsDeviceIndex
           << ",\"http_device_index\":" << httpDeviceIndex
           << ",\"features_device_index\":" << featuresDeviceIndex
           << ",\"cuda_read\":" << CaseLogger::Bool(cudaRead)
           << ",\"hash_match\":" << CaseLogger::Bool(hashMatch)
           << ",\"diagnostics_fetched\":" << CaseLogger::Bool(diagnosticsFetched)
           << ",\"diagnostics_missing_events\":" << CaseLogger::Array(missingDiagnostics)
           << ",\"width\":" << ctx.state.cudaShare.width
           << ",\"height\":" << ctx.state.cudaShare.height
           << ",\"channels\":" << ctx.state.cudaShare.channels
           << ",\"dtype\":" << CaseLogger::Quote(ctx.state.cudaShare.dtype)
           << ",\"input_sha256\":" << CaseLogger::Quote(inputHash)
           << ",\"output_sha256\":" << CaseLogger::Quote(outputHash)
           << ",\"expected_output_sha256\":" << CaseLogger::Quote(expectedOutputHash);
    actual << ",\"cuda_available\":" << CaseLogger::Bool(cudaAvailable);
    if (!cudaAvailable && !cudaAvailableError.empty())
    {
        actual << ",\"cuda_unavailable_error\":" << CaseLogger::Quote(cudaAvailableError);
    }
    if (!cudaReadError.empty())
    {
        actual << ",\"cuda_read_error\":" << CaseLogger::Quote(cudaReadError);
    }
    if (!infoError.empty())
    {
        actual << ",\"cuda_info_error\":" << CaseLogger::Quote(infoError);
    }
    if (!diagnosticsError.empty())
    {
        actual << ",\"diagnostics_error\":" << CaseLogger::Quote(diagnosticsError);
    }
    actual << "}";
    rec.expectedJson = "{\"selected\":true,\"terminal\":\"execution_success\",\"cuda_status\":true,\"shape_valid\":true,\"device_index_match\":true,\"hash_match\":true}";
    rec.actualJson = actual.str();
    rec.result = result;

    const int index = ctx.recorder.Record(rec);
    AppendDeliveryEvidence(index, caseId, ctx.http, ctx.logger, options, ctx.facadeLog, logStart, ctx.state,
                           negotiation.str(), "", "", hashes.str(), diagnosticsJson);
    if (infoOk && !infoResponse.body.empty())
    {
        ctx.logger.AppendCaseFile(index, caseId, "cuda-share-info.json", CaseLogger::PrettyPrint(infoResponse.body) + "\n");
    }
}

void WriteResultFile(const std::string& outputRoot, const std::string& phaseName, const MatrixSummary& summary)
{
    std::ostringstream json;
    json << "{"
         << "\"schema_version\":1,"
         << "\"profile\":\"conformance\","
         << "\"phase\":" << CaseLogger::Quote(phaseName) << ","
         << "\"result\":" << CaseLogger::Quote(summary.Ok() ? "pass" : "fail") << ","
         << "\"totals\":{"
         << "\"pass\":" << summary.passed << ","
         << "\"fail\":" << summary.failed << ","
         << "\"skip\":" << summary.skipped << ","
         << "\"error\":" << summary.errored << "}";
    if (!summary.setupFailureCode.empty())
    {
        json << ",\"setup_failure_code\":" << CaseLogger::Quote(summary.setupFailureCode);
    }
    json << "}";

    std::ofstream stream(outputRoot + "/conformance-result.json");
    stream << CaseLogger::PrettyPrint(json.str()) << "\n";
}

// Facade-behavior assertions against the live server (spec §facade): the things
// the suite could not catch while it bypassed the Client facade — pre-flight
// validation of a missing required input, and contract caching feeding the
// stateful SelectOutputTransport. Correlation is asserted inside the http deliver
// case (a delivered OutputReady must carry the correlated consumer_id).
void RunFacadeBehaviorCase(DeliveryContext& ctx)
{
    const MatrixOptions& options = ctx.options;
    CaseRecord rec;
    rec.caseId = "facade-behavior";
    rec.title = "Client facade: contract caching + pre-flight validation";
    rec.phase = options.phase == Phase::DeliveryRemote ? "delivery-remote" : "delivery-local";
    rec.description = "Drives Client::ParseWorkflow (caches the contract), then asserts the stateful "
                      "SelectOutputTransport resolves from that cache and Submit's pre-flight blocks a missing "
                      "required input before any inject round trip.";
    rec.specRef = kSpecFacade;
    ctx.facadeLog.SetCase("facade-behavior");

    const std::string workflowJson = BuildRoundTripWorkflowJson("STRING", "file_path");
    cec::Result<cec::WorkflowContract> parsed = ctx.client.ParseWorkflow(workflowJson);
    ctx.facadeLog.Action("parse_workflow",
                         parsed ? ("\"outputs\":" + std::to_string(parsed.Value().outputs.size())) : "",
                         parsed ? "ok" : "fail", parsed ? "" : parsed.GetError().message);
    if (!parsed || parsed.Value().outputs.empty())
    {
        rec.result = "error";
        rec.actualJson = "{\"parse_ok\":false}";
        rec.errors.push_back("parse_contract_mismatch");
        ctx.recorder.Record(rec);
        return;
    }

    // Caching: the stateful overload pulls type-allowed from the cached contract
    // and server-available from the negotiated session; only the policy is ours.
    cec::TransportPolicy policy;
    policy.reachable_transports.push_back("http");
    policy.reachable_transports.push_back("disk");
    policy.preference_order.push_back("http");
    policy.preference_order.push_back("disk");
    const std::string outputName = parsed.Value().outputs[0].name;
    cec::OutputTransportDecision cached = ctx.client.SelectOutputTransport(workflowJson, outputName, policy);
    ctx.facadeLog.Action("select_output_transport.cached",
                         "\"output\":" + CaseLogger::Quote(outputName) + ",\"transport\":" +
                             CaseLogger::Quote(cached.transport),
                         cached.ok ? "ok" : "fail", cached.error);
    const bool cachingOk = cached.ok && !cached.usable_transports.empty();

    // Pre-flight validation: omit the required input; Submit must fail client-side
    // with the missing-input list, before any HTTP inject.
    cec::SubmitRequest missing;
    missing.workflow_json = workflowJson;
    missing.client_id = options.clientId;
    missing.consumer_id = "facade-behavior";
    missing.execute = false;
    missing.output = cec::OutputRequest::Http("file_path", "bin");
    cec::Result<cec::JobHandle> blocked = ctx.client.Submit(missing);
    const bool preflightOk = !blocked && !blocked.GetError().missing_required_inputs.empty();
    ctx.facadeLog.Action("submit.preflight",
                         "\"missing\":" + CaseLogger::Array(blocked.GetError().missing_required_inputs),
                         preflightOk ? "ok" : "fail", blocked ? "submit unexpectedly accepted" : "");

    const bool ok = cachingOk && preflightOk;
    std::ostringstream actual;
    actual << "{\"caching_ok\":" << CaseLogger::Bool(cachingOk)
           << ",\"cached_usable\":" << CaseLogger::Array(cached.usable_transports)
           << ",\"preflight_blocked\":" << CaseLogger::Bool(!blocked)
           << ",\"missing_required_inputs\":" << CaseLogger::Array(blocked.GetError().missing_required_inputs) << "}";
    rec.expectedJson = "{\"caching_ok\":true,\"preflight_blocked\":true}";
    rec.actualJson = actual.str();
    rec.result = ok ? "pass" : "fail";
    if (!cachingOk)
    {
        rec.errors.push_back("contract_cache_select_failed");
    }
    if (!preflightOk)
    {
        rec.errors.push_back("preflight_validation_missing");
    }
    const int index = ctx.recorder.Record(rec);
    if (!ctx.facadeLog.Lines().empty())
    {
        ctx.logger.AppendCaseFile(index, "facade-behavior", "facade-trace.log", ctx.facadeLog.Lines());
    }
}

// Negotiation phase (Layer 1): discovery, compatibility, readiness decision, the
// transport-selection matrix (driven through the facade), and the WS handshake.
void RunNegotiationCases(
    CaseRecorder& recorder,
    cec::Client& client,
    IWebSocketProbe& ws,
    FacadeLog& facadeLog,
    const MatrixOptions& options,
    const cec::ServerFacts& server,
    const cec::Result<cec::Session>& session,
    bool wsConnected)
{
    // 3. Server/plugin protocol compatibility, from the facade handshake.
    {
        const bool ok = static_cast<bool>(session);
        std::ostringstream actual;
        actual << "{\"ok\":" << CaseLogger::Bool(ok)
               << ",\"error\":" << CaseLogger::Quote(ok ? "" : session.GetError().message)
               << ",\"server_protocol_version\":" << CaseLogger::Quote(server.protocol_version.ToString())
               << ",\"server_supports_protocol\":" << CaseLogger::Quote(server.supports_protocol.ToString())
               << "}";
        CaseRecord rec;
        rec.caseId = "server-protocol-compatibility";
        rec.title = "Plugin/server protocol range is compatible with the C++ client";
        rec.phase = "liveness";
        rec.description = "Client::Connect() gates on protocol compatibility over live /features facts: the C++ "
                          "client and plugin/server protocol ranges intersect.";
        rec.specRef = kSpecLiveness;
        rec.expectedJson = "{\"ok\":true}";
        rec.actualJson = actual.str();
        rec.result = ok ? "pass" : "fail";
        if (!ok)
        {
            rec.errors.push_back("server_feature_mismatch");
        }
        recorder.Record(rec);
    }

    // 4. Live type-axis assertions via Client::ParseWorkflow, when a fixture exists.
    if (options.parseWorkflowJson.empty())
    {
        CaseRecord rec;
        rec.caseId = "parse-type-axis";
        rec.title = "Type-axis discovery skipped (no parse workflow fixture)";
        rec.phase = "discovery";
        rec.description = "No parse workflow fixture was supplied, so the output type->transport axis was not exercised live.";
        rec.specRef = kSpecTypeAxis;
        rec.actualJson = "{\"reason\":\"no_parse_workflow\"}";
        rec.result = "skip";
        recorder.Record(rec);
    }
    else
    {
        facadeLog.SetCase("parse-type-axis");
        cec::Result<cec::WorkflowContract> contract = client.ParseWorkflow(options.parseWorkflowJson);
        facadeLog.Action("parse_workflow",
                         contract ? ("\"outputs\":" + std::to_string(contract.Value().outputs.size())) : "",
                         contract ? "ok" : "fail", contract ? "" : contract.GetError().message);
        if (!contract)
        {
            CaseRecord rec;
            rec.caseId = "parse-type-axis";
            rec.title = "Type-axis discovery failed: /notch/parse did not parse";
            rec.phase = "discovery";
            rec.description = "Client::ParseWorkflow could not produce a contract from the /notch/parse response.";
            rec.specRef = kSpecTypeAxis;
            rec.actualJson = "{\"error\":" + CaseLogger::Quote(contract.GetError().message) + "}";
            rec.result = "error";
            rec.errors.push_back("parse_contract_mismatch");
            recorder.Record(rec);
        }
        else
        {
            for (size_t i = 0; i < contract.Value().outputs.size(); ++i)
            {
                const cec::WorkflowOutput& output = contract.Value().outputs[i];
                std::vector<std::string> expected = ExpectedTypeTransports(output.type);
                bool ok = Sorted(expected) == Sorted(output.transports);

                CaseRecord rec;
                rec.caseId = "parse-output-" + output.name;
                rec.title = "Output '" + output.name + "' (" + output.type + ") reports its type-allowed transports";
                rec.phase = "discovery";
                rec.description = "/notch/parse must report the type->transport set for this connected output.";
                rec.specRef = kSpecTypeAxis;
                rec.expectedJson = "{\"type\":" + CaseLogger::Quote(output.type) + ",\"transports\":" + CaseLogger::Array(expected) + "}";
                rec.actualJson = "{\"type\":" + CaseLogger::Quote(output.type) + ",\"transports\":" + CaseLogger::Array(output.transports) + "}";
                rec.result = ok ? "pass" : "fail";
                if (!ok)
                {
                    rec.errors.push_back("parse_contract_mismatch");
                }
                recorder.Record(rec);
            }
        }
    }

    // 4b. Deployment-readiness gate via Client::GetRequiredFiles.
    if (options.requiredFilesReadyJson.empty() && options.requiredFilesMissingJson.empty())
    {
        CaseRecord rec;
        rec.caseId = "deployment-readiness";
        rec.title = "Deployment-readiness skipped (no required-files fixture)";
        rec.phase = "readiness";
        rec.description = "No required-files workflow fixture was supplied, so the deployment-readiness gate was not exercised live.";
        rec.specRef = kSpecReadiness;
        rec.actualJson = "{\"reason\":\"no_required_files_fixture\"}";
        rec.result = "skip";
        recorder.Record(rec);
    }
    else
    {
        if (!options.requiredFilesReadyJson.empty())
        {
            RunReadinessCase(recorder, client, facadeLog, "deployment-ready",
                "All required input files present -> ready",
                "Every file-backed input the workflow references exists on the server, so the run is ready.",
                options.requiredFilesReadyJson, true);
        }
        if (!options.requiredFilesMissingJson.empty())
        {
            RunReadinessCase(recorder, client, facadeLog, "deployment-missing-file",
                "A required input file is missing -> not ready (client would block)",
                "The workflow references a file the server does not have; the readiness decision is not-ready and the client would block the run.",
                options.requiredFilesMissingJson, false);
        }
    }

    // 5. Transport-selection matrix, driven through Client::SelectOutputTransport.
    for (const SelectionRow& row : kHardRows)
    {
        RunSelectionRow(recorder, client, facadeLog, row, /*soft=*/false);
    }
    for (const SelectionRow& row : kSoftRows)
    {
        RunSelectionRow(recorder, client, facadeLog, row, /*soft=*/true);
    }

    // 6. WebSocket handshake: the harness connected the socket and Client::Connect
    //    announced the feature flags on it. Re-announce via the facade and drain.
    {
        if (!wsConnected)
        {
            CaseRecord rec;
            rec.caseId = "ws-handshake";
            rec.title = "WebSocket /ws handshake + catch-up frames";
            rec.phase = "liveness";
            rec.description = "Connect to /ws, send the feature-flags message, and drain catch-up frames.";
            rec.specRef = kSpecLiveness;
            rec.expectedJson = "{\"connected\":true,\"sent\":true}";
            rec.actualJson = "{\"connected\":false}";
            rec.result = "fail";
            rec.errors.push_back("websocket_timeout");
            recorder.Record(rec);
        }
        else
        {
            facadeLog.SetCase("ws-handshake");
            cec::Result<bool> sent = client.SendFeatureFlags();
            facadeLog.Action("send_feature_flags", "", sent ? "ok" : "fail", sent ? "" : sent.GetError().message);
            std::vector<std::string> frames = ws.DrainReceived();
            std::ostringstream actual;
            actual << "{\"connected\":true,\"sent\":" << CaseLogger::Bool(static_cast<bool>(sent))
                   << ",\"received_frames\":" << frames.size() << "}";
            CaseRecord rec;
            rec.caseId = "ws-handshake";
            rec.title = "WebSocket /ws handshake + catch-up frames";
            rec.phase = "liveness";
            rec.description = "Connect to /ws, send the feature-flags message, and drain catch-up frames. Proves the WebSocket transport works end to end.";
            rec.specRef = kSpecLiveness;
            rec.expectedJson = "{\"connected\":true,\"sent\":true}";
            rec.actualJson = actual.str();
            rec.result = sent ? "pass" : "fail";
            if (!sent)
            {
                rec.errors.push_back("websocket_send_failed");
            }
            recorder.Record(rec);
        }
    }
}

std::string RejectAxisName(const std::string& verdict)
{
    if (verdict == "reject-type") return "type";
    if (verdict == "reject-server") return "server";
    if (verdict == "reject-client") return "client";
    return "unknown";
}

std::string DeliveryCaseTitle(const DeliveryCaseDescriptor& d)
{
    if (d.verdict == "deliver")
    {
        return d.outputType + " output over " + d.transport + " (" + d.topology + ")";
    }
    return d.outputType + " over " + d.transport + " rejected by " + RejectAxisName(d.verdict) + " (" + d.topology + ")";
}

// A reject case is pure client-side negotiation through the facade: the client
// computes usable = type-allowed n server-available n client-reachable and must
// refuse the requested transport before any inject (no silent downgrade).
void RunRejectCase(
    DeliveryContext& ctx,
    const DeliveryCaseDescriptor& descriptor,
    const std::vector<std::string>& serverAvailable,
    const std::vector<std::string>& clientReachable)
{
    const std::vector<std::string> typeAllowed = TypeAllowedTransports(*descriptor.type);
    cec::OutputTransportRequest request =
        MakeTransportRequest(typeAllowed, serverAvailable, clientReachable, std::vector<std::string>{},
                             descriptor.transport);
    cec::OutputTransportDecision decision = ctx.client.SelectOutputTransport(request);
    ctx.facadeLog.SetCase(descriptor.id);
    ctx.facadeLog.Action("select_output_transport", "\"transport\":" + CaseLogger::Quote(decision.transport),
                         decision.ok ? "ok" : "fail", decision.error);

    const bool ok = !decision.ok;
    CaseRecord rec;
    rec.caseId = descriptor.id;
    rec.title = DeliveryCaseTitle(descriptor);
    rec.phase = ctx.options.phase == Phase::DeliveryRemote ? "delivery-remote" : "delivery-local";
    rec.description = "The " + descriptor.outputType + " output is not deliverable over " + descriptor.transport +
                      " in the " + descriptor.topology + " topology (excluded by " +
                      RejectAxisName(descriptor.verdict) +
                      "), so the client must refuse the hard request before inject — no silent downgrade.";
    rec.specRef = ctx.options.phase == Phase::DeliveryRemote ? kSpecDeliveryRemote : kSpecDeliveryLocal;
    rec.requiredTransport = descriptor.transport;
    rec.expectedJson = "{\"selected\":false,\"reject_axis\":\"" + RejectAxisName(descriptor.verdict) + "\"}";
    rec.actualJson = "{\"selected\":" + CaseLogger::Bool(decision.ok) + ",\"choice\":" + DecisionJson(decision) + "}";
    rec.result = ok ? "pass" : "fail";
    if (!ok)
    {
        rec.errors.push_back("unexpected_accept");
    }
    const int index = ctx.recorder.Record(rec);
    std::ostringstream negotiation;
    negotiation << "{\"type_allowed\":" << CaseLogger::Array(typeAllowed)
                << ",\"server_available\":" << CaseLogger::Array(serverAvailable)
                << ",\"client_reachable\":" << CaseLogger::Array(clientReachable)
                << ",\"required_transport\":" << CaseLogger::Quote(descriptor.transport)
                << ",\"choice\":" << DecisionJson(decision) << "}";
    ctx.logger.AppendCaseFile(index, descriptor.id, "negotiation.json", CaseLogger::PrettyPrint(negotiation.str()) + "\n");
}

void RunSkipCase(DeliveryContext& ctx, const DeliveryCaseDescriptor& descriptor, const std::string& reason)
{
    CaseRecord rec;
    rec.caseId = descriptor.id;
    rec.title = DeliveryCaseTitle(descriptor);
    rec.phase = ctx.options.phase == Phase::DeliveryRemote ? "delivery-remote" : "delivery-local";
    rec.description = "Skipped: " + reason + ". The case is generated and tracked; it self-clears when the capability is present.";
    rec.specRef = ctx.options.phase == Phase::DeliveryRemote ? kSpecDeliveryRemote : kSpecDeliveryLocal;
    rec.requiredTransport = descriptor.transport;
    rec.actualJson = "{\"reason\":" + CaseLogger::Quote(reason) + "}";
    rec.result = "skip";
    rec.errors.push_back(reason);
    ctx.recorder.Record(rec);
}

void RunTopologyCases(DeliveryContext& ctx, const TopologyConfig& topology, bool useNamedRouteDisk,
                      const std::string& skipReason)
{
    for (const DeliveryCaseDescriptor& descriptor : GenerateDeliveryCases(topology))
    {
        if (!skipReason.empty())
        {
            RunSkipCase(ctx, descriptor, skipReason);
            continue;
        }
        if (descriptor.verdict != "deliver")
        {
            RunRejectCase(ctx, descriptor, topology.serverAvailable, topology.clientReachable);
            continue;
        }
        if (descriptor.transport == "cuda")
        {
            RunCudaDeliveryCase(ctx, descriptor.id, DeliveryCaseTitle(descriptor),
                                "Inject an image, execute, deliver over CUDA, and verify the share contract and "
                                "(where IPC is supported) the imported bytes.");
            continue;
        }
        RunFilePathDeliveryCase(ctx, descriptor.id, DeliveryCaseTitle(descriptor),
                                "Inject the " + descriptor.outputType + " input, execute, deliver over " +
                                    descriptor.transport + ", retrieve the artifact, and verify it by its class.",
                                descriptor.transport, useNamedRouteDisk, descriptor.type);
    }
}

void RunDeliveryCases(DeliveryContext& ctx)
{
    const std::vector<std::string> server = DeliveryServerTransports(ctx.server);

    // Facade-behavior assertions run once per delivery phase (caching + pre-flight).
    RunFacadeBehaviorCase(ctx);

    if (ctx.options.phase == Phase::DeliveryLocal)
    {
        const TopologyConfig local{"local", server, std::vector<std::string>{"cuda", "disk", "http"}};
        RunTopologyCases(ctx, local, false, "");
        return;
    }

    if (ctx.options.phase == Phase::DeliveryRemote)
    {
        const TopologyConfig remoteHttp{"remote-http", server, std::vector<std::string>{"http"}};
        RunTopologyCases(ctx, remoteHttp, false, "");

        const TopologyConfig routeDisk{"remote-route-disk", server, std::vector<std::string>{"disk", "http"}};
        const std::string routeSkip =
            NamedRouteConfiguredForClient(ctx.options, ctx.server) ? "" : "named_route_unsupported";
        RunTopologyCases(ctx, routeDisk, true, routeSkip);
    }
}

const char* PhaseName(Phase phase)
{
    switch (phase)
    {
    case Phase::Negotiation: return "negotiation";
    case Phase::DeliveryLocal: return "delivery_local";
    case Phase::DeliveryRemote: return "delivery_remote";
    }
    return "negotiation";
}

std::string JoinCsv(const std::vector<std::string>& values)
{
    return CaseLogger::Array(values);
}

} // namespace

MatrixSummary RunConformance(
    cec::HttpTransport& http,
    IWebSocketProbe& ws,
    const MatrixOptions& options,
    CaseLogger& logger,
    ICudaShareReader* cudaReader)
{
    MatrixSummary summary;
    CaseRecorder recorder(logger, summary);
    FacadeLog facadeLog(logger, PhaseName(options.phase));

    DeliveryRunState liveState;
    DeliveryEventSink sink(liveState, facadeLog);

    cec::ClientOptions clientOptions;
    clientOptions.client_id = options.clientId;
    clientOptions.http_transport = &http;
    clientOptions.web_socket_transport = &ws;
    clientOptions.event_sink = &sink;
    cec::Client client(clientOptions);

    // 1. Record the linked-against C++ client facts.
    cec::CppClientFacts clientFacts = cec::Client::facts();
    {
        std::ostringstream json;
        json << "{\"record\":\"cpp_client_compatibility_facts\""
             << ",\"cpp_client_version\":" << CaseLogger::Quote(clientFacts.cpp_client_version.ToString())
             << ",\"cpp_api_version\":" << CaseLogger::Quote(clientFacts.cpp_api_version.ToString())
             << ",\"protocol_version\":" << CaseLogger::Quote(clientFacts.protocol_version.ToString())
             << ",\"supports_protocol\":" << CaseLogger::Quote(clientFacts.supports_protocol.ToString())
             << ",\"capabilities\":" << JoinCsv(clientFacts.capabilities.values)
             << ",\"handled_notch_websocket_events\":" << JoinCsv(clientFacts.handled_notch_websocket_events)
             << ",\"source_git_commit\":" << CaseLogger::Quote(clientFacts.source_git_commit)
             << ",\"source_git_tag\":" << CaseLogger::Quote(clientFacts.source_git_tag) << "}";
        logger.Event(json.str());
    }

    // 2. Shared setup: discover server facts via the facade (live GET /features).
    cec::Result<cec::ServerFacts> discovered = client.Discover();
    facadeLog.Action("discover", discovered ? "" : "", discovered ? "ok" : "fail",
                     discovered ? "" : discovered.GetError().message);
    if (!discovered)
    {
        summary.setupFailureCode = "harness_error";
        logger.Event("{\"record\":\"setup_failure\",\"stage\":\"discover\",\"error\":" +
                     CaseLogger::Quote(discovered.GetError().message) + "}");
        WriteResultFile(options.outputRoot, PhaseName(options.phase), summary);
        return summary;
    }
    const cec::ServerFacts server = discovered.Value();
    {
        std::ostringstream json;
        json << "{\"record\":\"plugin_server_facts\""
             << ",\"plugin_version\":" << CaseLogger::Quote(server.plugin_version.ToString())
             << ",\"protocol_version\":" << CaseLogger::Quote(server.protocol_version.ToString())
             << ",\"supports_protocol\":" << CaseLogger::Quote(server.supports_protocol.ToString())
             << ",\"minimum_client_protocol\":" << CaseLogger::Quote(server.minimum_client_protocol.ToString())
             << ",\"plugin_capabilities\":" << JoinCsv(server.capabilities.values)
             << ",\"tested_comfyui_refs\":" << JoinCsv(server.tested_comfyui_refs)
             << ",\"output_transports\":" << JoinCsv(server.output_transports)
             << ",\"cuda_device_index\":" << server.cuda_device_index << "}";
        logger.Event(json.str());
    }

    // 3. Open the WebSocket (harness owns the socket; the facade is threadless),
    //    then Connect() through the facade: it re-discovers, gates protocol
    //    compatibility, and announces the client's feature flags on the socket.
    std::string wsError;
    const bool wsConnected = ws.Connect(options.wsTimeoutMs, wsError);
    cec::Result<cec::Session> session = client.Connect();
    facadeLog.Action("connect", "", session ? "ok" : "fail", session ? "" : session.GetError().message);

    if (options.phase == Phase::Negotiation)
    {
        RunNegotiationCases(recorder, client, ws, facadeLog, options, server, session, wsConnected);
    }
    else if (!session)
    {
        summary.setupFailureCode = "server_feature_mismatch";
        logger.Event("{\"record\":\"setup_failure\",\"stage\":\"connect\",\"error\":" +
                     CaseLogger::Quote(session.GetError().message) + "}");
        ws.Close();
        WriteResultFile(options.outputRoot, PhaseName(options.phase), summary);
        return summary;
    }
    else
    {
        DeliveryContext ctx{recorder, client,    http,   ws,        logger,    facadeLog,
                            sink,     liveState, options, server,   cudaReader};
        RunDeliveryCases(ctx);
    }

    ws.Close();
    logger.WriteIndex();
    WriteResultFile(options.outputRoot, PhaseName(options.phase), summary);
    return summary;
}

} // namespace notch_mock
