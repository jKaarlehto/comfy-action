#include "matrix.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "delivery_types.h"
#include "hash_utils.h"
#include "verify/verify_byte_exact.h"
#include "verify/verify_integrity.h"
#include "verify/verify_structural.h"

namespace notch_mock
{

using notch_comfy::ClientProtocol;

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

std::string ChoiceJson(const notch_comfy::OutputTransportChoice& choice)
{
    std::ostringstream json;
    json << "{\"ok\":" << CaseLogger::Bool(choice.m_ok)
         << ",\"transport\":" << CaseLogger::Quote(choice.m_transport)
         << ",\"usable\":" << CaseLogger::Array(choice.m_usableTransports);
    if (!choice.m_error.empty())
    {
        json << ",\"error\":" << CaseLogger::Quote(choice.m_error);
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

void RunSelectionRow(CaseRecorder& recorder, const SelectionRow& row, bool soft)
{
    notch_comfy::OutputTransportOptions options;
    options.m_typeAllowedTransports = Split(row.typeAllowed);
    options.m_serverAvailableTransports = Split(row.serverAvailable);
    options.m_clientReachableTransports = Split(row.clientReachable);
    if (soft)
    {
        options.m_preferenceOrder = Split(row.requiredOrPreference);
    }
    else
    {
        options.m_requiredTransport = row.requiredOrPreference;
    }

    notch_comfy::OutputTransportChoice choice = ClientProtocol::SelectOutputTransport(options);

    bool ok = choice.m_ok == row.expectOk && (!row.expectOk || choice.m_transport == row.expectTransport);

    CaseRecord rec;
    rec.caseId = row.id;
    rec.title = row.title;
    rec.phase = "transport-selection";
    rec.description = row.description;
    rec.specRef = soft ? kSpecSoft : kSpecHard;
    if (soft)
    {
        rec.preferredOrder = Split(row.requiredOrPreference);
    }
    else
    {
        rec.requiredTransport = row.requiredOrPreference;
    }
    std::ostringstream fixture;
    fixture << "{\"type_allowed\":" << CaseLogger::Array(Split(row.typeAllowed))
            << ",\"server_available\":" << CaseLogger::Array(Split(row.serverAvailable))
            << ",\"client_reachable\":" << CaseLogger::Array(Split(row.clientReachable)) << "}";
    rec.fixtureJson = fixture.str();
    rec.expectedJson = ExpectJson(row.expectOk, row.expectTransport);
    rec.actualJson = ChoiceJson(choice);
    rec.result = ok ? "pass" : "fail";
    if (!ok)
    {
        rec.errors.push_back("transport_selection_mismatch");
    }
    recorder.Record(rec);
}

// Deployment-readiness decision (Layer 1, no execution): POST
// /notch/get-required-files and compute the *client-side* decision — any
// required file with exists=false ⇒ not-ready ⇒ the client would block the run.
// This verifies the readiness decision only. The facts are server-published
// deployment state; the decision is user-actionable, not auto-negotiated, and is
// orthogonal to transport selection.
void RunReadinessCase(
    CaseRecorder& recorder,
    notch_comfy::IHttpTransport& http,
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

    notch_comfy::HttpResponse response;
    std::string sendError;
    if (!http.Send(ClientProtocol::BuildRequiredFilesRequest(workflowJson), response, sendError))
    {
        rec.result = "error";
        rec.actualJson = "{\"error\":" + CaseLogger::Quote(sendError) + "}";
        rec.errors.push_back("harness_error");
        recorder.Record(rec);
        return;
    }
    std::vector<notch_comfy::RequiredFile> files;
    std::string parseError;
    if (!ClientProtocol::ParseRequiredFilesResponse(response.m_body, files, parseError))
    {
        rec.result = "error";
        rec.actualJson = "{\"error\":" + CaseLogger::Quote(parseError) + "}";
        rec.errors.push_back("required_files_parse_error");
        recorder.Record(rec);
        return;
    }
    int missing = 0;
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (!files[i].m_exists)
        {
            ++missing;
        }
    }
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

bool NamedRouteConfiguredForClient(const MatrixOptions& options,
                                   const notch_comfy::ServerDeploymentFacts& deployment)
{
    return !options.namedRouteId.empty() && !options.namedRouteClientRoot.empty() &&
           ContainsString(deployment.m_namedDiskRouteIds, options.namedRouteId);
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

std::vector<std::string> DeliveryServerTransports(const notch_comfy::ServerDeploymentFacts& deployment)
{
    std::vector<std::string> transports;
    for (size_t i = 0; i < deployment.m_outputTransports.size(); ++i)
    {
        const std::string transport = deployment.m_outputTransports[i];
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
    const notch_comfy::ServerDeploymentFacts& deployment)
{
    if (phase != Phase::DeliveryRemote)
    {
        return std::vector<std::string>{"cuda", "disk", "http"};
    }
    if (namedRouteDisk && NamedRouteConfiguredForClient(options, deployment))
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

notch_comfy::OutputTransportKind TransportKindFromName(const std::string& transport)
{
    if (transport == "disk")
    {
        return notch_comfy::OutputTransportDisk;
    }
    if (transport == "http")
    {
        return notch_comfy::OutputTransportHttp;
    }
    if (transport == "cuda")
    {
        return notch_comfy::OutputTransportCuda;
    }
    return notch_comfy::OutputTransportUnset;
}

struct DeliveryRunState
{
    bool queued = false;
    std::string promptId;
    std::string injectError;
    std::string terminalType;
    bool terminalSuccess = false;
    bool outputReady = false;
    bool cudaStatus = false;
    notch_comfy::OutputReady output;
    notch_comfy::CudaShareStatus cudaShare;
    std::vector<std::string> websocketFrames;
};

bool WaitForDeliveryEvents(
    IWebSocketProbe& ws,
    const std::string& promptId,
    const std::string& consumerId,
    const std::string& transport,
    int timeoutMs,
    DeliveryRunState& state)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::vector<std::string> batch = ws.DrainReceived();
        for (size_t i = 0; i < batch.size(); ++i)
        {
            state.websocketFrames.push_back(batch[i]);
            notch_comfy::WebSocketEvent event;
            std::string eventError;
            if (!ClientProtocol::ParseWebSocketEvent(batch[i], event, eventError))
            {
                continue;
            }
            if (event.m_kind == notch_comfy::EventNotchOutputReady)
            {
                std::vector<notch_comfy::OutputReady> outputs;
                std::string outputError;
                if (ClientProtocol::ParseOutputReadyEvent(batch[i], outputs, outputError))
                {
                    for (size_t j = 0; j < outputs.size(); ++j)
                    {
                        if ((outputs[j].m_promptId.empty() || outputs[j].m_promptId == promptId) &&
                            outputs[j].m_name == consumerId &&
                            outputs[j].m_transport == transport)
                        {
                            state.output = outputs[j];
                            state.outputReady = true;
                        }
                    }
                }
            }
            if (event.m_kind == notch_comfy::EventNotchCudaShareStatus)
            {
                std::vector<notch_comfy::CudaShareStatus> shares;
                std::string cudaError;
                if (ClientProtocol::ParseCudaShareStatusEvent(batch[i], shares, cudaError))
                {
                    for (size_t j = 0; j < shares.size(); ++j)
                    {
                        if (shares[j].m_name == consumerId)
                        {
                            state.cudaShare = shares[j];
                            state.cudaStatus = true;
                        }
                    }
                }
            }
            if (!event.m_promptId.empty() && event.m_promptId != promptId)
            {
                continue;
            }
            if (event.m_kind == notch_comfy::EventExecutionSuccess)
            {
                state.terminalType = "execution_success";
                state.terminalSuccess = true;
            }
            else if (event.m_kind == notch_comfy::EventExecutionError ||
                     event.m_kind == notch_comfy::EventExecutionInterrupted)
            {
                state.terminalType = event.m_type;
            }
        }

        if (state.terminalSuccess && (transport == "cuda" ? state.cudaStatus : state.outputReady))
        {
            return true;
        }
        if (!state.terminalType.empty() && !state.terminalSuccess)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
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

std::string OutputReadyJson(const notch_comfy::OutputReady& output)
{
    std::ostringstream json;
    json << "{\"name\":" << CaseLogger::Quote(output.m_name)
         << ",\"prompt_id\":" << CaseLogger::Quote(output.m_promptId)
         << ",\"transport\":" << CaseLogger::Quote(output.m_transport)
         << ",\"path\":" << CaseLogger::Quote(output.m_path)
         << ",\"named_route_id\":" << CaseLogger::Quote(output.m_namedRouteId)
         << ",\"relative_path\":" << CaseLogger::Quote(output.m_relativePath)
         << ",\"url\":" << CaseLogger::Quote(output.m_url)
         << ",\"type\":" << CaseLogger::Quote(output.m_type)
         << ",\"format\":" << CaseLogger::Quote(output.m_format)
         << ",\"width\":" << output.m_width
         << ",\"height\":" << output.m_height << "}";
    return json.str();
}

std::string CudaShareStatusJson(const notch_comfy::CudaShareStatus& share)
{
    std::ostringstream json;
    json << "{\"name\":" << CaseLogger::Quote(share.m_name)
         << ",\"width\":" << share.m_width
         << ",\"height\":" << share.m_height
         << ",\"channels\":" << share.m_channels
         << ",\"pixel_format\":" << CaseLogger::Quote(share.m_pixelFormat)
         << ",\"dtype\":" << CaseLogger::Quote(share.m_dtype)
         << ",\"channel_order\":" << CaseLogger::Quote(share.m_channelOrder)
         << ",\"memory_layout\":" << CaseLogger::Quote(share.m_memoryLayout)
         << ",\"row_stride_bytes\":" << share.m_rowStrideBytes
         << ",\"data_offset\":" << share.m_dataOffset
         << ",\"data_size_bytes\":" << share.m_dataSizeBytes
         << ",\"metadata_offset\":" << share.m_metadataOffset
         << ",\"metadata_size_bytes\":" << share.m_metadataSizeBytes
         << ",\"frame_counter_offset\":" << share.m_frameCounterOffset
         << ",\"cuda_device_index\":" << share.m_cudaDeviceIndex
         << ",\"notch_consumer_id\":" << CaseLogger::Quote(share.m_notchConsumerId)
         << ",\"resource_id\":" << CaseLogger::Quote(share.m_resourceId)
         << ",\"has_ipc_handle\":" << CaseLogger::Bool(!share.m_ipcHandleHex.empty())
         << ",\"has_event_ipc_handle\":" << CaseLogger::Bool(!share.m_eventIpcHandleHex.empty())
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

void AppendPromptDiagnostics(
    int index,
    const std::string& caseId,
    notch_comfy::IHttpTransport& http,
    CaseLogger& logger,
    const std::string& promptId)
{
    if (promptId.empty())
    {
        return;
    }
    notch_comfy::HttpRequest diagRequest;
    diagRequest.m_method = "GET";
    diagRequest.m_path = "/notch/diagnostics?prompt_id=" + promptId;
    diagRequest.m_contentType = "application/json";
    notch_comfy::HttpResponse diagResponse;
    std::string diagError;
    if (http.Send(diagRequest, diagResponse, diagError) && !diagResponse.m_body.empty())
    {
        logger.AppendCaseFile(index, caseId, "notch-diagnostics.json",
                              CaseLogger::PrettyPrint(diagResponse.m_body) + "\n");
    }
}

bool FetchPromptDiagnostics(
    notch_comfy::IHttpTransport& http,
    const std::string& promptId,
    std::string& body,
    std::string& error)
{
    body.clear();
    error.clear();
    if (promptId.empty())
    {
        error = "prompt_id is empty";
        return false;
    }

    notch_comfy::HttpRequest diagRequest;
    diagRequest.m_method = "GET";
    diagRequest.m_path = "/notch/diagnostics?prompt_id=" + promptId;
    diagRequest.m_contentType = "application/json";
    notch_comfy::HttpResponse diagResponse;
    std::string sendError;
    if (!http.Send(diagRequest, diagResponse, sendError))
    {
        error = sendError.empty() ? "diagnostics request failed" : sendError;
        return false;
    }
    body = diagResponse.m_body;
    if (diagResponse.m_statusCode != 200)
    {
        error = "diagnostics endpoint returned HTTP " + std::to_string(diagResponse.m_statusCode);
        return false;
    }
    return true;
}

std::vector<std::string> MissingDiagnosticsEvents(
    const std::string& diagnosticsJson,
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
    notch_comfy::IHttpTransport& http,
    CaseLogger& logger,
    const MatrixOptions& options,
    const long logStart,
    const DeliveryRunState& state,
    const std::string& negotiationJson,
    const std::string& injectRequestJson,
    const std::string& injectResponseJson,
    const std::string& hashesJson,
    const std::string& diagnosticsJson = "")
{
    logger.AppendCaseFile(index, caseId, "negotiation.json", CaseLogger::PrettyPrint(negotiationJson) + "\n");
    if (!injectRequestJson.empty())
    {
        // Multipart requests carry binary form data, not JSON. Pretty-printing
        // binary as JSON is meaningless (and historically drove the formatter to
        // crash); write a small summary for multipart, pretty JSON otherwise.
        const bool looksJson = injectRequestJson[0] == '{' || injectRequestJson[0] == '[';
        if (looksJson)
        {
            logger.AppendCaseFile(index, caseId, "inject-request.json",
                                  CaseLogger::PrettyPrint(injectRequestJson) + "\n");
        }
        else
        {
            std::ostringstream note;
            note << "{\"note\":\"multipart/form-data body omitted (binary)\",\"bytes\":"
                 << injectRequestJson.size() << "}";
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

// The on-disk output extension the server uses for a given output type. Workflow-
// derived types carry their own format (a path's own extension, a File3D's glb);
// transcoded types are encoded into a fixed container.
// The server's output routes guard the consumer/output id with ^[A-Za-z0-9_-]+$
// (resolve_output_artifact / _SAFE_ID), so the http GET 404s on any other
// character. Generated case ids use '.' as a separator, so derive a safe
// consumer id (dots and any other disallowed char -> '-') for output routing
// while the dotted case id stays the human-facing record id.
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
    CaseRecorder& recorder,
    notch_comfy::IHttpTransport& http,
    IWebSocketProbe& ws,
    CaseLogger& logger,
    const MatrixOptions& options,
    const notch_comfy::ServerDeploymentFacts& deployment,
    const std::string& caseId,
    const std::string& title,
    const std::string& description,
    const std::string& transport,
    bool expectSuccess,
    bool useNamedRouteDisk = false,
    const DeliveryTypeContract* contractPtr = nullptr)
{
    // Default to the file_path contract (the historical behavior) when none given.
    static const DeliveryTypeContract kFilePathContract{
        "file_path", "STRING", "file_path", "", false, VerificationClass::ByteExact};
    const DeliveryTypeContract& contract = contractPtr ? *contractPtr : kFilePathContract;
    CaseRecord rec;
    rec.caseId = caseId;
    rec.title = title;
    rec.phase = options.phase == Phase::DeliveryRemote ? "delivery-remote" : "delivery-local";
    rec.description = description;
    rec.specRef = options.phase == Phase::DeliveryRemote ? kSpecDeliveryRemote : kSpecDeliveryLocal;
    rec.requiredTransport = transport;

    const std::vector<std::string> clientReachable =
        ClientReachableTransports(options.phase, useNamedRouteDisk, options, deployment);
    notch_comfy::OutputTransportOptions selectionOptions;
    selectionOptions.m_typeAllowedTransports = TypeAllowedTransports(contract);
    selectionOptions.m_serverAvailableTransports = DeliveryServerTransports(deployment);
    selectionOptions.m_clientReachableTransports = clientReachable;
    selectionOptions.m_requiredTransport = transport;
    notch_comfy::OutputTransportChoice choice = ClientProtocol::SelectOutputTransport(selectionOptions);

    std::ostringstream negotiation;
    negotiation << "{\"type_allowed\":" << CaseLogger::Array(selectionOptions.m_typeAllowedTransports)
                << ",\"server_available\":" << CaseLogger::Array(selectionOptions.m_serverAvailableTransports)
                << ",\"client_reachable\":" << CaseLogger::Array(selectionOptions.m_clientReachableTransports)
                << ",\"server_named_route_ids\":" << CaseLogger::Array(deployment.m_namedDiskRouteIds)
                << ",\"named_route_id\":" << CaseLogger::Quote(useNamedRouteDisk ? options.namedRouteId : "")
                << ",\"required_transport\":" << CaseLogger::Quote(transport)
                << ",\"choice\":" << ChoiceJson(choice) << "}";

    if (!expectSuccess)
    {
        const bool ok = !choice.m_ok;
        rec.expectedJson = "{\"selected\":false}";
        rec.actualJson = "{\"selected\":" + CaseLogger::Bool(choice.m_ok) + ",\"choice\":" + ChoiceJson(choice) + "}";
        rec.result = ok ? "pass" : "fail";
        if (!ok)
        {
            rec.errors.push_back("unexpected_accept");
        }
        const int index = recorder.Record(rec);
        DeliveryRunState emptyState;
        AppendDeliveryEvidence(index, caseId, http, logger, options, -1, emptyState, negotiation.str(), "", "", "");
        return;
    }

    // load3d_camera is injected as an inline JSON value (no fixture file); every
    // other type has a source file: file_path uses it as a server-staged path,
    // the rest upload its bytes by multipart.
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
            recorder.Record(rec);
            return;
        }
    }

    if (!choice.m_ok)
    {
        rec.expectedJson = "{\"selected\":true,\"terminal\":\"execution_success\",\"hash_match\":true}";
        rec.actualJson = "{\"selected\":false,\"choice\":" + ChoiceJson(choice) + "}";
        rec.result = "fail";
        rec.errors.push_back("unexpected_reject");
        const int index = recorder.Record(rec);
        DeliveryRunState emptyState;
        AppendDeliveryEvidence(index, caseId, http, logger, options, -1, emptyState, negotiation.str(), "", "", "");
        return;
    }

    const std::string consumerId = SafeConsumerId(caseId);
    const std::string workflowJson = BuildRoundTripWorkflowJson(contract.inputType, contract.slotName);
    notch_comfy::WorkflowSubmissionRequest req;
    req.m_workflowJson = workflowJson;
    req.m_clientId = options.clientId;
    req.m_consumerId = consumerId;
    req.m_execute = true;
    req.m_broadcastWs = true;
    // Input transport per the contract: file_path passes a server-staged path
    // string; load3d_camera passes an inline JSON dict; every other type uploads
    // the fixture bytes by multipart so the bytes really travel client->server.
    if (contract.outputType == "file_path")
    {
        req.m_inputs.push_back(notch_comfy::InputValue::String("test_input", sourcePath, contract.inputType));
    }
    else if (inlineInput)
    {
        req.m_inputs.push_back(notch_comfy::InputValue::Json("test_input", kCameraInlineJson, contract.inputType));
    }
    else
    {
        const std::string uploadName = contract.fixtureFile.empty() ? "input.bin" : contract.fixtureFile;
        req.m_inputs.push_back(notch_comfy::InputValue::Binary("test_input", sourceBytes, uploadName, contract.inputType));
    }
    req.m_output.m_transport = TransportKindFromName(transport);
    req.m_output.m_type = contract.outputType;
    req.m_output.m_extension = OutputExtensionForType(contract.outputType, sourcePath);
    if (transport == "disk")
    {
        if (useNamedRouteDisk)
        {
            req.m_output.m_namedRouteId = options.namedRouteId;
            req.m_output.m_relativeDirectory = NamedRouteRelativeDirectory(options);
        }
        else
        {
            req.m_output.m_path = DefaultLocalOutputPath(options);
        }
        req.m_output.m_filenamePrefix = caseId;
    }

    std::string wsError;
    if (!ws.Connect(options.wsTimeoutMs, wsError))
    {
        rec.result = "error";
        rec.expectedJson = "{\"websocket_connected\":true}";
        rec.actualJson = "{\"websocket_connected\":false,\"error\":" + CaseLogger::Quote(wsError) + "}";
        rec.errors.push_back("websocket_timeout");
        recorder.Record(rec);
        return;
    }

    notch_comfy::WorkflowSubmissionBuildResult built = ClientProtocol::BuildWorkflowSubmissionRequest(req);
    if (!built.m_ok)
    {
        ws.Close();
        rec.result = "error";
        rec.actualJson = "{\"build_error\":" + CaseLogger::Quote(built.m_error) + "}";
        rec.errors.push_back("harness_error");
        recorder.Record(rec);
        return;
    }

    const long logStart = FileSize(ServerLogPath(options));
    notch_comfy::HttpResponse injectResponse;
    std::string sendError;
    if (!http.Send(built.m_request, injectResponse, sendError))
    {
        ws.Close();
        rec.result = "error";
        rec.actualJson = "{\"inject_error\":" + CaseLogger::Quote(sendError) + "}";
        rec.errors.push_back("harness_error");
        recorder.Record(rec);
        return;
    }

    notch_comfy::WorkflowSubmissionResult submission;
    std::string parseError;
    const bool parsed = ClientProtocol::ParseWorkflowSubmissionResponse(injectResponse.m_body, submission, parseError);
    DeliveryRunState state;
    state.queued = parsed && submission.m_queued;
    state.promptId = submission.m_promptId;
    state.injectError = submission.m_error.empty() ? parseError : submission.m_error;

    if (state.queued)
    {
        WaitForDeliveryEvents(ws, state.promptId, consumerId, transport, options.executeTimeoutMs, state);
    }
    ws.Close();

    std::vector<uint8_t> outputBytes;
    std::string outputPath;
    std::string outputError;
    bool outputRead = false;
    int outputHttpStatus = 0;  // captured for the http transport so a 404/500 is visible in the case
    if (state.outputReady)
    {
        if (transport == "disk")
        {
            if (useNamedRouteDisk)
            {
                if (state.output.m_namedRouteId != options.namedRouteId)
                {
                    outputError = "named_route_id mismatch";
                }
                else if (!IsSafeRelativePath(state.output.m_relativePath))
                {
                    outputError = "unsafe or empty relative_path";
                }
                else
                {
                    outputPath = JoinPath(options.namedRouteClientRoot, state.output.m_relativePath);
                    outputRead = ReadBinaryFile(outputPath, outputBytes, outputError);
                }
            }
            else
            {
                outputPath = state.output.m_path;
                outputRead = ReadBinaryFile(outputPath, outputBytes, outputError);
            }
        }
        else if (transport == "http")
        {
            notch_comfy::HttpRequest getRequest;
            getRequest.m_method = "GET";
            getRequest.m_path = state.output.m_url;
            notch_comfy::HttpResponse getResponse;
            std::string getError;
            const bool sent = http.Send(getRequest, getResponse, getError);
            outputHttpStatus = getResponse.m_statusCode;
            if (sent && getResponse.m_statusCode == 200)
            {
                outputBytes.assign(getResponse.m_body.begin(), getResponse.m_body.end());
                outputRead = true;
                outputPath = state.output.m_url;
            }
            else if (!sent)
            {
                outputError = getError.empty() ? "http output fetch transport error" : getError;
            }
            else
            {
                outputError = "http output fetch returned HTTP " + std::to_string(getResponse.m_statusCode) +
                              " for " + state.output.m_url;
            }
        }
    }

    const std::string inputHash = sourceBytes.empty() ? "" : Sha256Bytes(sourceBytes);
    const std::string outputHash = outputRead ? Sha256Bytes(outputBytes) : "";
    // Verify the delivered artifact by the type's verification class (byte-exact
    // for file_path, structural for file_3d/camera, integrity for audio/video/mesh,
    // decoded-as-integrity for image until stb lands). A clean output read is a
    // precondition; the verifier decides the match.
    const VerifyResult verify = outputRead ? RunVerifier(contract, sourceBytes, outputBytes) : VerifyResult{};
    const bool verifyOk = outputRead && verify.ok;
    // The named-route handoff only applies to the *disk* transport. In the
    // remote-route-disk topology http is also usable, and an http delivery returns
    // a url (not a {route_id, relative_path}); checking the named-route handoff on
    // an http case would wrongly fail an otherwise-good delivery.
    const bool routeHandoffApplies = useNamedRouteDisk && transport == "disk";
    const bool namedRouteHandoffOk = !routeHandoffApplies ||
        (state.outputReady && state.output.m_namedRouteId == options.namedRouteId &&
         IsSafeRelativePath(state.output.m_relativePath) && state.output.m_path.empty());
    std::string diagnosticsJson;
    std::string diagnosticsError;
    const bool diagnosticsFetched = state.promptId.empty()
        ? false
        : FetchPromptDiagnostics(http, state.promptId, diagnosticsJson, diagnosticsError);
    std::vector<std::string> requiredDiagnosticsEvents;
    if (state.queued)
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
    // Diagnostics are context, not a verdict gate (spec §9b: "diagnostics are
    // context, not pass/fail"). missingDiagnostics is recorded as evidence but
    // never fails a case.
    const std::vector<std::string> missingDiagnostics =
        diagnosticsFetched ? MissingDiagnosticsEvents(diagnosticsJson, requiredDiagnosticsEvents)
                           : requiredDiagnosticsEvents;
    const bool ok = state.queued && state.terminalSuccess && state.outputReady &&
                    namedRouteHandoffOk && verifyOk;
    if (!state.queued)
    {
        rec.errors.push_back("unexpected_reject");
    }
    if (state.queued && !state.terminalSuccess)
    {
        rec.errors.push_back(state.terminalType.empty() ? "websocket_timeout" : "execution_error");
    }
    if (state.terminalSuccess && !state.outputReady)
    {
        rec.errors.push_back("output_event_missing");
    }
    if (state.outputReady && !outputRead)
    {
        rec.errors.push_back("output_artifact_missing");
    }
    if (state.outputReady && !namedRouteHandoffOk)
    {
        rec.errors.push_back("named_route_handoff_mismatch");
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
           << ",\"named_route_id\":" << CaseLogger::Quote(state.output.m_namedRouteId)
           << ",\"relative_path\":" << CaseLogger::Quote(state.output.m_relativePath)
           << ",\"bytes\":" << outputBytes.size()
           << ",\"sha256\":" << CaseLogger::Quote(outputHash)
           << ",\"verification\":" << (verify.detail.empty() ? "null" : verify.detail) << "}]}";

    std::ostringstream actual;
    actual << "{\"selected_transport\":" << CaseLogger::Quote(choice.m_transport)
           << ",\"output_type\":" << CaseLogger::Quote(contract.outputType)
           << ",\"queued\":" << CaseLogger::Bool(state.queued)
           << ",\"prompt_id\":" << CaseLogger::Quote(state.promptId)
           << ",\"terminal\":" << CaseLogger::Quote(state.terminalType)
           << ",\"output_ready\":" << CaseLogger::Bool(state.outputReady)
           << ",\"output_read\":" << CaseLogger::Bool(outputRead)
           << ",\"output_url\":" << CaseLogger::Quote(state.output.m_url)
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
    rec.expectedJson = "{\"selected\":true,\"terminal\":\"execution_success\",\"output_ready\":true,\"named_route_handoff_ok\":true,\"verify_ok\":true}";
    rec.actualJson = actual.str();
    rec.result = ok ? "pass" : "fail";

    const int index = recorder.Record(rec);
    AppendDeliveryEvidence(index, caseId, http, logger, options, logStart, state,
                           negotiation.str(), built.m_request.m_body, injectResponse.m_body, hashes.str(),
                           diagnosticsJson);
}

void RunCudaDeliveryCase(
    CaseRecorder& recorder,
    notch_comfy::IHttpTransport& http,
    IWebSocketProbe& ws,
    CaseLogger& logger,
    const MatrixOptions& options,
    const notch_comfy::ServerDeploymentFacts& deployment,
    ICudaShareReader* cudaReader,
    const std::string& caseId,
    const std::string& title,
    const std::string& description,
    bool expectSuccess)
{
    CaseRecord rec;
    rec.caseId = caseId;
    rec.title = title;
    rec.phase = options.phase == Phase::DeliveryRemote ? "delivery-remote" : "delivery-local";
    rec.description = description;
    rec.specRef = options.phase == Phase::DeliveryRemote ? kSpecDeliveryRemote : kSpecDeliveryLocal;
    rec.requiredTransport = "cuda";

    const std::vector<std::string> clientReachable = ClientReachableTransports(options.phase);
    notch_comfy::OutputTransportOptions selectionOptions;
    selectionOptions.m_typeAllowedTransports = std::vector<std::string>{"cuda", "disk", "http"};
    selectionOptions.m_serverAvailableTransports = DeliveryServerTransports(deployment);
    selectionOptions.m_clientReachableTransports = clientReachable;
    selectionOptions.m_requiredTransport = "cuda";
    notch_comfy::OutputTransportChoice choice = ClientProtocol::SelectOutputTransport(selectionOptions);

    std::ostringstream negotiation;
    negotiation << "{\"type_allowed\":" << CaseLogger::Array(selectionOptions.m_typeAllowedTransports)
                << ",\"server_available\":" << CaseLogger::Array(selectionOptions.m_serverAvailableTransports)
                << ",\"client_reachable\":" << CaseLogger::Array(selectionOptions.m_clientReachableTransports)
                << ",\"required_transport\":\"cuda\""
                << ",\"choice\":" << ChoiceJson(choice) << "}";

    if (!expectSuccess)
    {
        const bool ok = !choice.m_ok;
        rec.expectedJson = "{\"selected\":false}";
        rec.actualJson = "{\"selected\":" + CaseLogger::Bool(choice.m_ok) + ",\"choice\":" + ChoiceJson(choice) + "}";
        rec.result = ok ? "pass" : "fail";
        if (!ok)
        {
            rec.errors.push_back("unexpected_accept");
        }
        const int index = recorder.Record(rec);
        DeliveryRunState emptyState;
        AppendDeliveryEvidence(index, caseId, http, logger, options, -1, emptyState, negotiation.str(), "", "", "");
        return;
    }

    if (!choice.m_ok || !ContainsString(selectionOptions.m_serverAvailableTransports, "cuda") ||
        deployment.m_cudaDeviceIndex < 0)
    {
        rec.expectedJson = "{\"cuda_available\":true}";
        rec.actualJson = "{\"cuda_available\":false,\"choice\":" + ChoiceJson(choice) + "}";
        rec.result = "skip";
        rec.errors.push_back("cuda_unavailable");
        const int index = recorder.Record(rec);
        DeliveryRunState emptyState;
        AppendDeliveryEvidence(index, caseId, http, logger, options, -1, emptyState, negotiation.str(), "", "", "");
        return;
    }

    if (cudaReader == nullptr)
    {
        rec.expectedJson = "{\"cuda_reader_available\":true}";
        rec.actualJson = "{\"cuda_reader_available\":false}";
        rec.result = "skip";
        rec.errors.push_back("cuda_reader_unavailable");
        recorder.Record(rec);
        return;
    }

    const int width = 8;
    const int height = 8;
    std::vector<uint8_t> imageBytes = BuildFloat32RgbPattern(width, height);
    std::vector<uint8_t> expectedCudaBytes = BuildExpectedFloat32RgbaPattern(width, height);

    const std::string consumerId = SafeConsumerId(caseId);
    const std::string workflowJson = BuildRoundTripWorkflowJson("IMAGE", "image");
    notch_comfy::WorkflowSubmissionRequest req;
    req.m_workflowJson = workflowJson;
    req.m_clientId = options.clientId;
    req.m_consumerId = consumerId;
    req.m_execute = true;
    req.m_broadcastWs = true;
    notch_comfy::InputValue imageInput =
        notch_comfy::InputValue::Binary("test_input", imageBytes, "cuda_input.float32rgb", "IMAGE");
    imageInput.m_rawBuffer.m_enabled = true;
    imageInput.m_rawBuffer.m_width = width;
    imageInput.m_rawBuffer.m_height = height;
    imageInput.m_rawBuffer.m_format = "float32_rgb";
    imageInput.m_rawBuffer.m_stride = width * 3 * 4;
    req.m_inputs.push_back(imageInput);
    req.m_output.m_transport = notch_comfy::OutputTransportCuda;
    req.m_output.m_type = "image";

    std::string wsError;
    if (!ws.Connect(options.wsTimeoutMs, wsError))
    {
        rec.result = "error";
        rec.expectedJson = "{\"websocket_connected\":true}";
        rec.actualJson = "{\"websocket_connected\":false,\"error\":" + CaseLogger::Quote(wsError) + "}";
        rec.errors.push_back("websocket_timeout");
        recorder.Record(rec);
        return;
    }

    notch_comfy::WorkflowSubmissionBuildResult built = ClientProtocol::BuildWorkflowSubmissionRequest(req);
    if (!built.m_ok)
    {
        ws.Close();
        rec.result = "error";
        rec.actualJson = "{\"build_error\":" + CaseLogger::Quote(built.m_error) + "}";
        rec.errors.push_back("harness_error");
        recorder.Record(rec);
        return;
    }

    const long logStart = FileSize(ServerLogPath(options));
    notch_comfy::HttpResponse injectResponse;
    std::string sendError;
    if (!http.Send(built.m_request, injectResponse, sendError))
    {
        ws.Close();
        rec.result = "error";
        rec.actualJson = "{\"inject_error\":" + CaseLogger::Quote(sendError) + "}";
        rec.errors.push_back("harness_error");
        recorder.Record(rec);
        return;
    }

    notch_comfy::WorkflowSubmissionResult submission;
    std::string parseError;
    const bool parsed = ClientProtocol::ParseWorkflowSubmissionResponse(injectResponse.m_body, submission, parseError);
    DeliveryRunState state;
    state.queued = parsed && submission.m_queued;
    state.promptId = submission.m_promptId;
    state.injectError = submission.m_error.empty() ? parseError : submission.m_error;
    if (state.queued)
    {
        WaitForDeliveryEvents(ws, state.promptId, consumerId, "cuda", options.executeTimeoutMs, state);
    }
    // NOTE: keep the WebSocket OPEN through the share-info GET and the IPC import.
    // The server releases a client's CUDA output allocation on /ws disconnect
    // (api/websocket.py _handle_ws_disconnect), so closing here would free the
    // device allocation and make both the info endpoint 404 and the IPC handle
    // stale (CUDA_ERROR_INVALID_HANDLE). Close only after the bytes are read.

    notch_comfy::HttpRequest infoRequest;
    infoRequest.m_method = "GET";
    infoRequest.m_path = "/notch/cuda/share/" + consumerId;
    notch_comfy::HttpResponse infoResponse;
    std::string infoError;
    bool infoOk = http.Send(infoRequest, infoResponse, infoError) && infoResponse.m_statusCode == 200;

    // CUDA availability is the first skip gate. If CUDA is available, import
    // failure is normally a real failure (e.g. a wrong/malformed handle). The
    // only second skip is the evidenced WSL2 GPU-PV raw-IPC limitation below,
    // after the share contract and cross-channel handle integrity have passed.
    std::string cudaAvailableError;
    const bool cudaAvailable = cudaReader->CudaAvailable(cudaAvailableError);

    // Cross-channel handle integrity: the server publishes the IPC handle on two
    // independent channels (the WS notch-cuda-share-status and this HTTP info
    // endpoint, both via _alloc_to_info). The handle must be well-formed (64-byte
    // = 128-hex) and identical across both channels. A mismatch is a real
    // transport/serialization bug and always fails — even on WSL2.
    const std::string wsHandleHex = state.cudaShare.m_ipcHandleHex;
    const std::string httpHandleHex = infoOk ? ExtractJsonStringField(infoResponse.m_body, "ipc_handle") : "";
    const bool handleWellFormed = wsHandleHex.size() == 128;  // 64-byte IPC handle
    const bool handleIntegrityOk = handleWellFormed && !httpHandleHex.empty() && httpHandleHex == wsHandleHex;

    // Device-index coherence: the GPU the share lives on must agree across the WS
    // share-status, the HTTP info endpoint, and /features, and be a real device.
    // The reader independently verifies it imported on exactly that device (it
    // checks its own cudaSetDevice took effect). For a positive (an actual byte
    // transfer) this proves the share and the importer are on the same device and
    // context, not a coincidental match on a different GPU.
    const int wsDeviceIndex = state.cudaShare.m_cudaDeviceIndex;
    const int httpDeviceIndex = infoOk ? ExtractJsonIntField(infoResponse.m_body, "cuda_device_index", -1) : -1;
    const int featuresDeviceIndex = deployment.m_cudaDeviceIndex;
    const bool deviceIndexMatch =
        wsDeviceIndex >= 0 && wsDeviceIndex == httpDeviceIndex && wsDeviceIndex == featuresDeviceIndex;

    const std::string inputHash = Sha256Bytes(imageBytes);
    const std::string expectedOutputHash = Sha256Bytes(expectedCudaBytes);
    std::vector<uint8_t> cudaBytes;
    std::string cudaReadError;
    const bool cudaRead = state.cudaStatus && cudaReader->ReadShare(state.cudaShare, cudaBytes, cudaReadError);
    ws.Close();  // allocation no longer needed; safe to disconnect (releases the share)
    const std::string outputHash = cudaRead ? Sha256Bytes(cudaBytes) : "";
    const bool hashMatch = cudaRead && outputHash == expectedOutputHash;
    const bool shapeOk = state.cudaStatus && state.cudaShare.m_width == width && state.cudaShare.m_height == height &&
                         state.cudaShare.m_channels == 4 && state.cudaShare.m_dtype == "float32";
    std::string diagnosticsJson;
    std::string diagnosticsError;
    const bool diagnosticsFetched = state.promptId.empty()
        ? false
        : FetchPromptDiagnostics(http, state.promptId, diagnosticsJson, diagnosticsError);
    std::vector<std::string> requiredDiagnosticsEvents;
    if (state.queued)
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
    // Diagnostics are context, not a verdict gate (spec §9b). Recorded as
    // evidence, never failing a case.
    const std::vector<std::string> missingDiagnostics =
        diagnosticsFetched ? MissingDiagnosticsEvents(diagnosticsJson, requiredDiagnosticsEvents)
                           : requiredDiagnosticsEvents;
    // Verdict:
    //   - CUDA not available in this environment      -> skip (cuda_unavailable)
    //   - share-status/shape/info contract broken     -> fail
    //   - WSL platform rejects a verified share handle -> skip (cuda_ipc_unsupported)
    //   - other available-but-failed import/read       -> fail (e.g. wrong handle)
    //   - import ok but bytes mismatch                 -> fail (corruption)
    //   - import ok and bytes match                    -> pass
    // The driver error is always captured in cuda_read_error / cuda_unavailable_error.
    // shareContractOk also requires cross-channel handle integrity: the WS-status
    // and HTTP-info handles must agree and be well-formed. A mismatch is a real
    // transport/serialization bug and fails regardless of platform.
    const bool shareContractOk = state.queued && state.terminalSuccess && state.cudaStatus && shapeOk && infoOk &&
                                 handleIntegrityOk && deviceIndexMatch;
    std::string result;
    if (!cudaAvailable)
    {
        result = "skip";
        rec.errors.push_back("cuda_unavailable");
    }
    else if (shareContractOk && !cudaRead && IsWslPlatform())
    {
        // Server produced a valid, integrity-checked CUDA share with correct
        // metadata, but this WSL-marked runner cannot import the IPC handle. The
        // raw CUDA IPC probe records whether this is a platform limitation. Skip
        // the byte round-trip here; the server-side share contract was already
        // verified and this clears automatically on a runner where IPC import
        // works.
        result = "skip";
        rec.errors.push_back("cuda_ipc_unsupported");
    }
    else
    {
        if (!state.queued)
        {
            rec.errors.push_back("unexpected_reject");
        }
        if (state.queued && !state.terminalSuccess)
        {
            rec.errors.push_back(state.terminalType.empty() ? "websocket_timeout" : "execution_error");
        }
        if (state.terminalSuccess && !state.cudaStatus)
        {
            rec.errors.push_back("cuda_status_missing");
        }
        if (state.cudaStatus && !shapeOk)
        {
            rec.errors.push_back("cuda_metadata_mismatch");
        }
        if (state.cudaStatus && !infoOk)
        {
            rec.errors.push_back("cuda_info_unavailable");
        }
        if (state.cudaStatus && infoOk && !handleIntegrityOk)
        {
            rec.errors.push_back("cuda_handle_integrity_mismatch");
        }
        if (state.cudaStatus && infoOk && !deviceIndexMatch)
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
           << ",\"width\":" << state.cudaShare.m_width
           << ",\"height\":" << state.cudaShare.m_height
           << ",\"channels\":" << state.cudaShare.m_channels
           << ",\"dtype\":" << CaseLogger::Quote(state.cudaShare.m_dtype)
           << ",\"data_size_bytes\":" << state.cudaShare.m_dataSizeBytes << "}]}";

    std::ostringstream actual;
    actual << "{\"selected_transport\":" << CaseLogger::Quote(choice.m_transport)
           << ",\"queued\":" << CaseLogger::Bool(state.queued)
           << ",\"prompt_id\":" << CaseLogger::Quote(state.promptId)
           << ",\"terminal\":" << CaseLogger::Quote(state.terminalType)
           << ",\"cuda_status\":" << CaseLogger::Bool(state.cudaStatus)
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
           << ",\"width\":" << state.cudaShare.m_width
           << ",\"height\":" << state.cudaShare.m_height
           << ",\"channels\":" << state.cudaShare.m_channels
           << ",\"dtype\":" << CaseLogger::Quote(state.cudaShare.m_dtype)
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

    const int index = recorder.Record(rec);
    AppendDeliveryEvidence(index, caseId, http, logger, options, logStart, state,
                           negotiation.str(), built.m_request.m_body, injectResponse.m_body, hashes.str(),
                           diagnosticsJson);
    if (infoOk && !infoResponse.m_body.empty())
    {
        logger.AppendCaseFile(index, caseId, "cuda-share-info.json", CaseLogger::PrettyPrint(infoResponse.m_body) + "\n");
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

// Negotiation phase (Layer 1): discovery, readiness decision, the pure
// transport-selection matrix, and the WS handshake check. No execution.
void RunNegotiationCases(
    CaseRecorder& recorder,
    notch_comfy::IHttpTransport& http,
    IWebSocketProbe& ws,
    const MatrixOptions& options,
    const notch_comfy::ServerCompatibilityFacts& serverCompat)
{
    // 3. Server wire-protocol compatibility.
    notch_comfy::CompatibilityCheckResult compat = ClientProtocol::CheckServerCompatibility(serverCompat);
    {
        std::ostringstream actual;
        actual << "{\"ok\":" << CaseLogger::Bool(compat.m_ok)
               << ",\"client_too_old\":" << CaseLogger::Bool(compat.m_clientTooOld)
               << ",\"server_too_old\":" << CaseLogger::Bool(compat.m_serverTooOld) << "}";
        CaseRecord rec;
        rec.caseId = "server-wire-compatibility";
        rec.title = "Server wire-protocol version is compatible with the client";
        rec.phase = "liveness";
        rec.description = "CheckServerCompatibility over live /features facts: neither the client nor the server is too old.";
        rec.specRef = kSpecLiveness;
        rec.expectedJson = "{\"ok\":true}";
        rec.actualJson = actual.str();
        rec.result = compat.m_ok ? "pass" : "fail";
        if (!compat.m_ok)
        {
            rec.errors.push_back("server_feature_mismatch");
        }
        recorder.Record(rec);
    }

    // 4. Live /notch/parse type-axis assertions, when a workflow fixture exists.
    if (options.parseWorkflowJson.empty())
    {
        CaseRecord rec;
        rec.caseId = "parse-type-axis";
        rec.title = "Type-axis discovery skipped (no parse workflow fixture)";
        rec.phase = "discovery";
        rec.description = "No /notch/parse workflow fixture was supplied, so the output type->transport axis was not exercised live.";
        rec.specRef = kSpecTypeAxis;
        rec.actualJson = "{\"reason\":\"no_parse_workflow\"}";
        rec.result = "skip";
        recorder.Record(rec);
    }
    else
    {
        notch_comfy::HttpResponse parseResponse;
        std::string sendError;
        if (!http.Send(ClientProtocol::BuildWorkflowDiscoveryRequest(options.parseWorkflowJson), parseResponse, sendError))
        {
            CaseRecord rec;
            rec.caseId = "parse-type-axis";
            rec.title = "Type-axis discovery failed: /notch/parse request error";
            rec.phase = "discovery";
            rec.description = "The HTTP transport could not deliver the /notch/parse request.";
            rec.specRef = kSpecTypeAxis;
            rec.actualJson = "{\"error\":" + CaseLogger::Quote(sendError) + "}";
            rec.result = "error";
            rec.errors.push_back("harness_error");
            recorder.Record(rec);
        }
        else
        {
            notch_comfy::WorkflowContract contract;
            std::string contractError;
            if (!ClientProtocol::ParseWorkflowContract(parseResponse.m_body, contract, contractError))
            {
                CaseRecord rec;
                rec.caseId = "parse-type-axis";
                rec.title = "Type-axis discovery failed: /notch/parse contract did not parse";
                rec.phase = "discovery";
                rec.description = "The /notch/parse response could not be parsed into a workflow contract.";
                rec.specRef = kSpecTypeAxis;
                rec.actualJson = "{\"error\":" + CaseLogger::Quote(contractError) + "}";
                rec.result = "error";
                rec.errors.push_back("parse_contract_mismatch");
                recorder.Record(rec);
            }
            else
            {
                for (size_t i = 0; i < contract.m_outputs.size(); ++i)
                {
                    const notch_comfy::ContractOutput& output = contract.m_outputs[i];
                    std::vector<std::string> expected = ExpectedTypeTransports(output.m_type);
                    bool ok = Sorted(expected) == Sorted(output.m_transports);

                    CaseRecord rec;
                    rec.caseId = "parse-output-" + output.m_name;
                    rec.title = "Output '" + output.m_name + "' (" + output.m_type + ") reports its type-allowed transports";
                    rec.phase = "discovery";
                    rec.description = "/notch/parse must report the type->transport set for this connected output (the type-allowed axis, from OUTPUT_TYPES_BY_TRANSPORT).";
                    rec.specRef = kSpecTypeAxis;
                    rec.expectedJson = "{\"type\":" + CaseLogger::Quote(output.m_type) + ",\"transports\":" + CaseLogger::Array(expected) + "}";
                    rec.actualJson = "{\"type\":" + CaseLogger::Quote(output.m_type) + ",\"transports\":" + CaseLogger::Array(output.m_transports) + "}";
                    rec.result = ok ? "pass" : "fail";
                    if (!ok)
                    {
                        rec.errors.push_back("parse_contract_mismatch");
                    }
                    recorder.Record(rec);
                }
            }
        }
    }

    // 4b. Deployment-readiness gate (required input files reachable on server).
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
            RunReadinessCase(recorder, http, "deployment-ready",
                "All required input files present -> ready",
                "Every file-backed input the workflow references exists on the server, so the run is ready.",
                options.requiredFilesReadyJson, true);
        }
        if (!options.requiredFilesMissingJson.empty())
        {
            RunReadinessCase(recorder, http, "deployment-missing-file",
                "A required input file is missing -> not ready (client would block)",
                "The workflow references a file the server does not have; the readiness decision is not-ready and the client would block the run.",
                options.requiredFilesMissingJson, false);
        }
    }

    // 5. Pure transport-selection matrix (deterministic, no server).
    for (const SelectionRow& row : kHardRows)
    {
        RunSelectionRow(recorder, row, /*soft=*/false);
    }
    for (const SelectionRow& row : kSoftRows)
    {
        RunSelectionRow(recorder, row, /*soft=*/true);
    }

    // 6. WebSocket handshake check: connect, send the feature-flags message,
    //    drain any catch-up frames. Proves the transport is not dead code.
    {
        std::string wsError;
        if (!ws.Connect(options.wsTimeoutMs, wsError))
        {
            CaseRecord rec;
            rec.caseId = "ws-handshake";
            rec.title = "WebSocket /ws handshake + catch-up frames";
            rec.phase = "liveness";
            rec.description = "Connect to /ws, send the feature-flags message, and drain catch-up frames. Proves the WebSocket transport works end to end.";
            rec.specRef = kSpecLiveness;
            rec.expectedJson = "{\"connected\":true,\"sent\":true}";
            rec.actualJson = "{\"connected\":false,\"error\":" + CaseLogger::Quote(wsError) + "}";
            rec.result = "fail";
            rec.errors.push_back("websocket_timeout");
            recorder.Record(rec);
        }
        else
        {
            std::string sendError;
            bool sent = ws.SendText(ClientProtocol::BuildFeatureFlagsMessage(), sendError);
            std::vector<std::string> frames = ws.DrainReceived();
            ws.Close();
            std::ostringstream actual;
            actual << "{\"connected\":true,\"sent\":" << CaseLogger::Bool(sent)
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
                rec.errors.push_back("websocket_timeout");
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

// A reject case is pure client-side negotiation: the client computes usable =
// type-allowed n server-available n client-reachable and must refuse the
// requested transport before any inject (no silent downgrade). This is the real
// SelectOutputTransport with the type's actual type-allowed set.
void RunRejectCase(
    CaseRecorder& recorder,
    CaseLogger& logger,
    const MatrixOptions& options,
    const DeliveryCaseDescriptor& descriptor,
    const std::vector<std::string>& serverAvailable,
    const std::vector<std::string>& clientReachable)
{
    notch_comfy::OutputTransportOptions selectionOptions;
    selectionOptions.m_typeAllowedTransports = TypeAllowedTransports(*descriptor.type);
    selectionOptions.m_serverAvailableTransports = serverAvailable;
    selectionOptions.m_clientReachableTransports = clientReachable;
    selectionOptions.m_requiredTransport = descriptor.transport;
    notch_comfy::OutputTransportChoice choice = ClientProtocol::SelectOutputTransport(selectionOptions);

    const bool ok = !choice.m_ok;
    CaseRecord rec;
    rec.caseId = descriptor.id;
    rec.title = DeliveryCaseTitle(descriptor);
    rec.phase = options.phase == Phase::DeliveryRemote ? "delivery-remote" : "delivery-local";
    rec.description = "The " + descriptor.outputType + " output is not deliverable over " + descriptor.transport +
                      " in the " + descriptor.topology + " topology (excluded by " +
                      RejectAxisName(descriptor.verdict) +
                      "), so the client must refuse the hard request before inject — no silent downgrade.";
    rec.specRef = options.phase == Phase::DeliveryRemote ? kSpecDeliveryRemote : kSpecDeliveryLocal;
    rec.requiredTransport = descriptor.transport;
    rec.expectedJson = "{\"selected\":false,\"reject_axis\":\"" + RejectAxisName(descriptor.verdict) + "\"}";
    rec.actualJson = "{\"selected\":" + CaseLogger::Bool(choice.m_ok) + ",\"choice\":" + ChoiceJson(choice) + "}";
    rec.result = ok ? "pass" : "fail";
    if (!ok)
    {
        rec.errors.push_back("unexpected_accept");
    }
    const int index = recorder.Record(rec);
    std::ostringstream negotiation;
    negotiation << "{\"type_allowed\":" << CaseLogger::Array(selectionOptions.m_typeAllowedTransports)
                << ",\"server_available\":" << CaseLogger::Array(serverAvailable)
                << ",\"client_reachable\":" << CaseLogger::Array(clientReachable)
                << ",\"required_transport\":" << CaseLogger::Quote(descriptor.transport)
                << ",\"choice\":" << ChoiceJson(choice) << "}";
    logger.AppendCaseFile(index, descriptor.id, "negotiation.json", CaseLogger::PrettyPrint(negotiation.str()) + "\n");
}

// Emit a self-clearing skip for a generated case (used when a whole topology is
// capability-gated off, e.g. remote-route-disk before the server advertises the
// named route).
void RunSkipCase(
    CaseRecorder& recorder,
    const MatrixOptions& options,
    const DeliveryCaseDescriptor& descriptor,
    const std::string& reason)
{
    CaseRecord rec;
    rec.caseId = descriptor.id;
    rec.title = DeliveryCaseTitle(descriptor);
    rec.phase = options.phase == Phase::DeliveryRemote ? "delivery-remote" : "delivery-local";
    rec.description = "Skipped: " + reason + ". The case is generated and tracked; it self-clears when the capability is present.";
    rec.specRef = options.phase == Phase::DeliveryRemote ? kSpecDeliveryRemote : kSpecDeliveryLocal;
    rec.requiredTransport = descriptor.transport;
    rec.actualJson = "{\"reason\":" + CaseLogger::Quote(reason) + "}";
    rec.result = "skip";
    rec.errors.push_back(reason);
    recorder.Record(rec);
}

// Run every generated case for one topology: deliver cases execute a real round
// trip (cuda via the share reader, disk/http via the typed artifact runner);
// reject cases assert client-side refusal. When skipReason is set the whole
// topology is emitted as self-clearing skips instead.
void RunTopologyCases(
    CaseRecorder& recorder,
    notch_comfy::IHttpTransport& http,
    IWebSocketProbe& ws,
    CaseLogger& logger,
    const MatrixOptions& options,
    const notch_comfy::ServerDeploymentFacts& deployment,
    ICudaShareReader* cudaReader,
    const TopologyConfig& topology,
    bool useNamedRouteDisk,
    const std::string& skipReason)
{
    for (const DeliveryCaseDescriptor& descriptor : GenerateDeliveryCases(topology))
    {
        if (!skipReason.empty())
        {
            RunSkipCase(recorder, options, descriptor, skipReason);
            continue;
        }
        if (descriptor.verdict != "deliver")
        {
            RunRejectCase(recorder, logger, options, descriptor, topology.serverAvailable, topology.clientReachable);
            continue;
        }
        if (descriptor.transport == "cuda")
        {
            RunCudaDeliveryCase(recorder, http, ws, logger, options, deployment, cudaReader, descriptor.id,
                                DeliveryCaseTitle(descriptor),
                                "Inject an image, execute, deliver over CUDA, and verify the share contract and "
                                "(where IPC is supported) the imported bytes.",
                                true);
            continue;
        }
        RunFilePathDeliveryCase(recorder, http, ws, logger, options, deployment, descriptor.id,
                                DeliveryCaseTitle(descriptor),
                                "Inject the " + descriptor.outputType + " input, execute, deliver over " +
                                    descriptor.transport + ", retrieve the artifact, and verify it by its class.",
                                descriptor.transport, true, useNamedRouteDisk, descriptor.type);
    }
}

void RunDeliveryCases(
    CaseRecorder& recorder,
    notch_comfy::IHttpTransport& http,
    IWebSocketProbe& ws,
    const MatrixOptions& options,
    CaseLogger& logger,
    const notch_comfy::ServerDeploymentFacts& deployment,
    ICudaShareReader* cudaReader)
{
    const std::vector<std::string> server = DeliveryServerTransports(deployment);

    if (options.phase == Phase::DeliveryLocal)
    {
        // local: client and server share host (cuda + disk + http reachable).
        const TopologyConfig local{"local", server, std::vector<std::string>{"cuda", "disk", "http"}};
        RunTopologyCases(recorder, http, ws, logger, options, deployment, cudaReader, local, false, "");
        return;
    }

    if (options.phase == Phase::DeliveryRemote)
    {
        // remote-http: two containers, network only — the client reaches http only.
        const TopologyConfig remoteHttp{"remote-http", server, std::vector<std::string>{"http"}};
        RunTopologyCases(recorder, http, ws, logger, options, deployment, cudaReader, remoteHttp, false, "");

        // remote-route-disk: a shared named-route volume adds disk reachability.
        // Self-clearing skip until the server advertises the route id in /features.
        const TopologyConfig routeDisk{"remote-route-disk", server, std::vector<std::string>{"disk", "http"}};
        const std::string routeSkip =
            NamedRouteConfiguredForClient(options, deployment) ? "" : "named_route_unsupported";
        RunTopologyCases(recorder, http, ws, logger, options, deployment, cudaReader, routeDisk, true, routeSkip);
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

} // namespace

MatrixSummary RunConformance(
    notch_comfy::IHttpTransport& http,
    IWebSocketProbe& ws,
    const MatrixOptions& options,
    CaseLogger& logger,
    ICudaShareReader* cudaReader)
{
    MatrixSummary summary;
    CaseRecorder recorder(logger, summary);

    // 1. Record the linked-against client interface facts. A version that
    //    disagrees with the checked-out extension is then diagnosable offline.
    notch_comfy::ClientCompatibilityFacts clientFacts = ClientProtocol::GetClientCompatibilityFacts();
    {
        std::ostringstream json;
        json << "{\"record\":\"client_compatibility_facts\""
             << ",\"interface_version\":" << CaseLogger::Quote(clientFacts.m_clientInterfaceVersion)
             << ",\"wire_protocol_version\":" << clientFacts.m_wireProtocolVersion
             << ",\"minimum_server_wire_protocol_version\":" << clientFacts.m_minimumServerWireProtocolVersion
             << ",\"source_git_commit\":" << CaseLogger::Quote(clientFacts.m_sourceGitCommit)
             << ",\"source_git_tag\":" << CaseLogger::Quote(clientFacts.m_sourceGitTag) << "}";
        logger.Event(json.str());
    }

    // 2. Shared setup: live GET /features must be reachable and parseable.
    notch_comfy::HttpResponse featuresResponse;
    std::string transportError;
    if (!http.Send(ClientProtocol::BuildServerFeaturesRequest(), featuresResponse, transportError))
    {
        summary.setupFailureCode = "harness_error";
        logger.Event("{\"record\":\"setup_failure\",\"stage\":\"features_request\",\"error\":" + CaseLogger::Quote(transportError) + "}");
        WriteResultFile(options.outputRoot, PhaseName(options.phase), summary);
        return summary;
    }

    notch_comfy::ServerCompatibilityFacts serverCompat;
    notch_comfy::ServerDeploymentFacts deployment;
    std::string parseError;
    if (!ClientProtocol::ParseServerCompatibilityFacts(featuresResponse.m_body, serverCompat, parseError) ||
        !ClientProtocol::ParseServerDeploymentFacts(featuresResponse.m_body, deployment, parseError))
    {
        summary.setupFailureCode = "harness_error";
        logger.Event("{\"record\":\"setup_failure\",\"stage\":\"features_parse\",\"error\":" + CaseLogger::Quote(parseError) + "}");
        WriteResultFile(options.outputRoot, PhaseName(options.phase), summary);
        return summary;
    }
    {
        std::ostringstream json;
        json << "{\"record\":\"server_facts\""
             << ",\"wire_protocol_version\":" << serverCompat.m_wireProtocolVersion
             << ",\"minimum_client_wire_protocol_version\":" << serverCompat.m_minimumClientWireProtocolVersion
             << ",\"output_transports\":" << CaseLogger::Array(deployment.m_outputTransports)
             << ",\"cuda_device_index\":" << deployment.m_cudaDeviceIndex << "}";
        logger.Event(json.str());
    }

    if (options.phase == Phase::Negotiation)
    {
        RunNegotiationCases(recorder, http, ws, options, serverCompat);
    }
    else
    {
        RunDeliveryCases(recorder, http, ws, options, logger, deployment, cudaReader);
    }

    logger.WriteIndex();
    WriteResultFile(options.outputRoot, PhaseName(options.phase), summary);
    return summary;
}

} // namespace notch_mock
