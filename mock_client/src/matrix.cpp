#include "matrix.h"

#include <algorithm>
#include <fstream>
#include <sstream>

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
    std::string requestedOrOrder; // single transport (hard) or csv order (soft)
    bool expectOk;
    std::string expectTransport;
};

// Boundary-complete hard-request matrix: 5 positives (cuda; disk and http per
// type class) + 5 rejects (cuda excluded independently by type, server, and
// client; disk excluded by client; empty intersection). The cuda reject is
// isolated to one axis per row, so server-unavailable (no GPU) and
// client-unreachable (remote) cuda rejections are distinct cases.
const SelectionRow kHardRows[] = {
    {"image-cuda-local", "IMAGE, local client requiring cuda -> cuda chosen",
     "Image allows cuda, server has a CUDA device, client is local: required cuda is usable and chosen.",
     "cuda,disk,http", "cuda,disk,http", "cuda,disk,http", "cuda", true, "cuda"},
    {"image-disk-without-cuda", "IMAGE, no server CUDA, requiring disk -> disk chosen",
     "Server has no CUDA; a required disk transport is type-allowed, available, and reachable, so it is chosen.",
     "cuda,disk,http", "disk,http", "disk,http", "disk", true, "disk"},
    {"image-http-only-client", "IMAGE, http-only client requiring http -> http chosen",
     "Remote http-only client: http is the only usable transport and the required http is chosen.",
     "cuda,disk,http", "disk,http", "http", "http", true, "http"},
    {"nonimage-rejects-cuda-by-type", "Non-image requiring cuda -> rejected by type",
     "cuda is not type-allowed for a non-image output, so a required cuda is rejected (no silent downgrade).",
     "disk,http", "cuda,disk,http", "cuda,disk,http", "cuda", false, ""},
    {"nonimage-disk-local", "Non-image, local client requiring disk -> disk chosen",
     "disk is type-allowed, server-available, and client-reachable; the required disk is chosen.",
     "disk,http", "disk,http", "disk,http", "disk", true, "disk"},
    {"nonimage-rejects-unreachable-disk", "Non-image requiring disk on http-only client -> rejected",
     "disk is not client-reachable from a remote http-only client, so the required disk is rejected (no downgrade).",
     "disk,http", "disk,http", "http", "disk", false, ""},
    {"filepath-http-only-client", "File path, http-only client requiring http -> http chosen",
     "Non-image file path: http is the only usable transport and the required http is chosen.",
     "disk,http", "disk,http", "http", "http", true, "http"},
    {"image-rejects-cuda-by-server", "IMAGE requiring cuda with no server CUDA -> rejected",
     "Server has no CUDA device, so a required cuda is not server-available and is rejected (no downgrade).",
     "cuda,disk,http", "disk,http", "cuda,disk,http", "cuda", false, ""},
    {"image-rejects-cuda-by-client", "IMAGE requiring cuda from a remote client -> rejected",
     "Client is not local (http-only), so a required cuda is not client-reachable and is rejected (no downgrade).",
     "cuda,disk,http", "cuda,disk,http", "http", "cuda", false, ""},
    {"empty-intersection-fails", "No transport type-allowed, available, and reachable -> rejected",
     "The three eligibility sets do not intersect, so selection fails: no usable transport.",
     "disk,http", "cuda", "cuda", "cuda", false, ""},
};

const SelectionRow kSoftRows[] = {
    {"prefer-cuda-then-disk", "Preference cuda->disk->http with cuda unusable -> disk chosen",
     "Soft preference: the first usable transport in the order wins; cuda is not usable so disk is chosen.",
     "cuda,disk,http", "disk,http", "disk,http", "cuda,disk,http", true, "disk"},
    {"no-order-match-but-usable", "Preference cuda->disk with neither usable, http remains -> http chosen",
     "No preferred transport is usable, so the helper falls back to the first usable transport (http).",
     "disk,http", "disk,http", "http", "cuda,disk", true, "http"},
};

const char* const kSpecHard = "notch_contract_matrix_spec.md §4 hard selection (A1 n A2 n A3, hard request)";
const char* const kSpecSoft = "notch_contract_matrix_spec.md §4 soft selection (A4 preference order)";
const char* const kSpecTypeAxis = "notch_contract_matrix_spec.md §4 type-axis discovery (A1)";
const char* const kSpecReadiness = "notch_contract_matrix_spec.md §4 deployment-readiness decision (A5)";
const char* const kSpecLiveness = "notch_contract_matrix_spec.md §4 setup/liveness (A0)";

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

    void Record(CaseRecord record)
    {
        record.index = ++m_index;
        if (record.result == "pass") ++m_summary.passed;
        else if (record.result == "fail") ++m_summary.failed;
        else if (record.result == "skip") ++m_summary.skipped;
        else ++m_summary.errored;
        m_logger.WriteCase(record);
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
        options.m_preferenceOrder = Split(row.requestedOrOrder);
    }
    else
    {
        options.m_preferredTransport = row.requestedOrOrder;
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
        rec.preferredOrder = Split(row.requestedOrOrder);
    }
    else
    {
        rec.requestedTransport = row.requestedOrOrder;
    }
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

void WriteResultFile(const std::string& outputRoot, const MatrixSummary& summary)
{
    std::ostringstream json;
    json << "{"
         << "\"schema_version\":1,"
         << "\"profile\":\"contract_matrix\","
         << "\"phase\":\"matrix_v1\","
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

    std::ofstream stream(outputRoot + "/contract-matrix-result.json");
    stream << CaseLogger::PrettyPrint(json.str()) << "\n";
}

} // namespace

MatrixSummary RunMatrixV1(
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
        WriteResultFile(options.outputRoot, summary);
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
        WriteResultFile(options.outputRoot, summary);
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

    logger.WriteIndex();
    WriteResultFile(options.outputRoot, summary);
    return summary;
}

} // namespace notch_mock
