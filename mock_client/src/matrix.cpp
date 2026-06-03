#include "matrix.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

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
};

const char* const kSpecHard = "notch_conformance_spec.md §4 hard selection (A1 n A2 n A3, hard request)";
const char* const kSpecSoft = "notch_conformance_spec.md §4 soft selection (A4 preference order)";
const char* const kSpecTypeAxis = "notch_conformance_spec.md §4 type-axis discovery (A1)";
const char* const kSpecReadiness = "notch_conformance_spec.md §4 deployment-readiness decision (A5)";
const char* const kSpecLiveness = "notch_conformance_spec.md §4 setup/liveness (A0)";
const char* const kSpecExecution = "notch_conformance_spec.md §9a execution lifecycle (WS terminal event)";
const char* const kSpecFileAvailability = "notch_conformance_spec.md §9 file-availability enforcement (A5 runtime)";

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
// This verifies the readiness decision only; the matching server-side run-block
// enforcement is a Layer-2 assertion (not yet built server-side). The facts are
// server-published deployment state; the decision is user-actionable, not
// auto-negotiated, and is orthogonal to transport selection.
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

// Delivery-phase case: submit a workflow with execute=true, wait for the matching
// terminal WS event by prompt_id, and assert the run outcome. For expectSuccess
// the run must reach execution_success; otherwise (file-availability enforcement)
// the run must be blocked — rejected at inject (not queued) or terminated with
// execution_error/interrupted — and must NOT succeed. Writes per-case evidence:
// the websocket frames, the server.log slice for the run, and the Notch
// diagnostics captured under this prompt_id.
void RunExecutionCase(
    CaseRecorder& recorder,
    notch_comfy::IHttpTransport& http,
    IWebSocketProbe& ws,
    CaseLogger& logger,
    const MatrixOptions& options,
    const std::string& caseId,
    const std::string& title,
    const std::string& description,
    const std::string& workflowJson,
    bool expectSuccess)
{
    CaseRecord rec;
    rec.caseId = caseId;
    rec.title = title;
    rec.phase = expectSuccess ? "execution" : "delivery";
    rec.description = description;
    rec.specRef = expectSuccess ? kSpecExecution : kSpecFileAvailability;
    rec.expectedJson = expectSuccess ? "{\"terminal\":\"execution_success\"}"
                                     : "{\"blocked\":true}";

    const std::string serverLog = options.outputRoot + "/comfyui.log";
    const long logStart = FileSize(serverLog);

    std::string wsError;
    if (!ws.Connect(options.wsTimeoutMs, wsError))
    {
        rec.result = "error";
        rec.actualJson = "{\"connected\":false,\"error\":" + CaseLogger::Quote(wsError) + "}";
        rec.errors.push_back("websocket_timeout");
        recorder.Record(rec);
        return;
    }

    notch_comfy::WorkflowSubmissionRequest req;
    req.m_workflowJson = workflowJson;
    req.m_clientId = options.clientId;
    req.m_execute = true;
    req.m_broadcastWs = true;
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
    const bool queued = parsed && submission.m_queued;
    const std::string promptId = submission.m_promptId;

    // Wait for the terminal WS event matching this prompt_id (only if queued).
    std::vector<std::string> frames;
    std::string terminalType;
    bool terminalSuccess = false;
    if (queued)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.executeTimeoutMs);
        bool terminal = false;
        while (!terminal && std::chrono::steady_clock::now() < deadline)
        {
            std::vector<std::string> batch = ws.DrainReceived();
            for (size_t i = 0; i < batch.size(); ++i)
            {
                frames.push_back(batch[i]);
                notch_comfy::WebSocketEvent event;
                std::string evError;
                if (!ClientProtocol::ParseWebSocketEvent(batch[i], event, evError))
                {
                    continue;
                }
                if (!event.m_promptId.empty() && event.m_promptId != promptId)
                {
                    continue;
                }
                if (event.m_kind == notch_comfy::EventExecutionSuccess)
                {
                    terminalType = "execution_success";
                    terminalSuccess = true;
                    terminal = true;
                    break;
                }
                if (event.m_kind == notch_comfy::EventExecutionError ||
                    event.m_kind == notch_comfy::EventExecutionInterrupted)
                {
                    terminalType = event.m_type;
                    terminal = true;
                    break;
                }
            }
            if (!terminal)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
    ws.Close();

    // Decide the verdict.
    const bool blocked = !queued || (!terminalType.empty() && !terminalSuccess);
    bool ok;
    if (expectSuccess)
    {
        ok = terminalSuccess;
        if (!ok)
        {
            rec.errors.push_back(queued ? (terminalType.empty() ? "websocket_timeout" : "execution_error")
                                        : "unexpected_reject");
        }
    }
    else
    {
        ok = blocked;
        if (!ok)
        {
            rec.errors.push_back("unexpected_accept");
        }
    }

    std::ostringstream actual;
    actual << "{\"queued\":" << CaseLogger::Bool(queued)
           << ",\"prompt_id\":" << CaseLogger::Quote(promptId)
           << ",\"terminal\":" << CaseLogger::Quote(terminalType)
           << ",\"blocked\":" << CaseLogger::Bool(blocked);
    if (!queued && !submission.m_error.empty())
    {
        actual << ",\"inject_error\":" << CaseLogger::Quote(submission.m_error);
    }
    actual << "}";
    rec.actualJson = actual.str();
    rec.result = ok ? "pass" : "fail";

    const int index = recorder.Record(rec);

    // Per-case evidence: WS frames, the server.log slice for this run, and the
    // Notch diagnostics captured under this prompt_id (context, not pass/fail).
    if (!frames.empty())
    {
        std::string wsLog;
        for (size_t i = 0; i < frames.size(); ++i)
        {
            wsLog += frames[i];
            wsLog += "\n";
        }
        logger.AppendCaseFile(index, caseId, "websocket.jsonl", wsLog);
    }
    const long logEnd = FileSize(serverLog);
    const std::string slice = ReadFileSlice(serverLog, logStart, logEnd);
    if (!slice.empty())
    {
        logger.AppendCaseFile(index, caseId, "server.log", slice);
    }
    if (!promptId.empty())
    {
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
// transport-selection matrix, and the WS handshake smoke. No execution.
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

    // 6. WebSocket handshake smoke: connect, send the feature-flags message,
    //    drain any catch-up frames. Proves the transport is not dead code.
    {
        std::string wsError;
        if (!ws.Connect(options.wsTimeoutMs, wsError))
        {
            CaseRecord rec;
            rec.caseId = "ws-handshake-smoke";
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
            rec.caseId = "ws-handshake-smoke";
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

// Delivery phase (Layer 2): submit a workflow for execution and assert the
// terminal WS event, and enforce file-availability (a workflow referencing a
// missing file must be blocked, not run). Each case attaches per-case evidence.
void RunDeliveryCases(
    CaseRecorder& recorder,
    notch_comfy::IHttpTransport& http,
    IWebSocketProbe& ws,
    const MatrixOptions& options,
    CaseLogger& logger)
{
    if (options.executeWorkflowJson.empty())
    {
        CaseRecord rec;
        rec.caseId = "workflow-execution";
        rec.title = "Workflow execution skipped (no execute workflow fixture)";
        rec.phase = "execution";
        rec.description = "No execute workflow fixture was supplied, so workflow execution was not exercised.";
        rec.specRef = kSpecExecution;
        rec.actualJson = "{\"reason\":\"no_execute_workflow\"}";
        rec.result = "skip";
        recorder.Record(rec);
    }
    else
    {
        RunExecutionCase(recorder, http, ws, logger, options, "workflow-execution",
            "A submitted workflow runs to execution_success",
            "Submit a workflow with execute=true and wait for the terminal WebSocket event; it must reach execution_success. This proves the inject -> queue -> execute -> success lifecycle.",
            options.executeWorkflowJson, /*expectSuccess=*/true);
    }

    if (options.executeMissingFileJson.empty())
    {
        CaseRecord rec;
        rec.caseId = "missing-input-file";
        rec.title = "File-availability enforcement skipped (no missing-file fixture)";
        rec.phase = "delivery";
        rec.description = "No missing-file fixture was supplied, so file-availability enforcement was not exercised.";
        rec.specRef = kSpecFileAvailability;
        rec.actualJson = "{\"reason\":\"no_missing_file_workflow\"}";
        rec.result = "skip";
        recorder.Record(rec);
    }
    else
    {
        RunExecutionCase(recorder, http, ws, logger, options, "missing-input-file",
            "A workflow referencing a missing input file is blocked",
            "Submit a workflow that references an input file the server does not have, with execute=true. The run must be blocked (rejected at inject or terminated with execution_error) and must not reach execution_success.",
            options.executeMissingFileJson, /*expectSuccess=*/false);
    }
}

const char* PhaseName(Phase phase)
{
    switch (phase)
    {
    case Phase::Negotiation: return "negotiation";
    case Phase::Delivery: return "delivery";
    case Phase::All: return "all";
    }
    return "all";
}

} // namespace

MatrixSummary RunConformance(
    notch_comfy::IHttpTransport& http,
    IWebSocketProbe& ws,
    const MatrixOptions& options,
    CaseLogger& logger)
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

    if (options.phase != Phase::Delivery)
    {
        RunNegotiationCases(recorder, http, ws, options, serverCompat);
    }
    if (options.phase != Phase::Negotiation)
    {
        RunDeliveryCases(recorder, http, ws, options, logger);
    }

    logger.WriteIndex();
    WriteResultFile(options.outputRoot, PhaseName(options.phase), summary);
    return summary;
}

} // namespace notch_mock
