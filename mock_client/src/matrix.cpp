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
    {"image-cuda-local", "cuda,disk,http", "cuda,disk,http", "cuda,disk,http", "cuda", true, "cuda"},
    {"image-disk-without-cuda", "cuda,disk,http", "disk,http", "disk,http", "disk", true, "disk"},
    {"image-http-only-client", "cuda,disk,http", "disk,http", "http", "http", true, "http"},
    {"nonimage-rejects-cuda-by-type", "disk,http", "cuda,disk,http", "cuda,disk,http", "cuda", false, ""},
    {"nonimage-disk-local", "disk,http", "disk,http", "disk,http", "disk", true, "disk"},
    {"nonimage-rejects-unreachable-disk", "disk,http", "disk,http", "http", "disk", false, ""},
    {"filepath-http-only-client", "disk,http", "disk,http", "http", "http", true, "http"},
    {"image-rejects-cuda-by-server", "cuda,disk,http", "disk,http", "cuda,disk,http", "cuda", false, ""},
    {"image-rejects-cuda-by-client", "cuda,disk,http", "cuda,disk,http", "http", "cuda", false, ""},
    {"empty-intersection-fails", "disk,http", "cuda", "cuda", "cuda", false, ""},
};

const SelectionRow kSoftRows[] = {
    {"prefer-cuda-then-disk", "cuda,disk,http", "disk,http", "disk,http", "cuda,disk,http", true, "disk"},
    {"no-order-match-but-usable", "disk,http", "disk,http", "http", "cuda,disk", true, "http"},
};

} // namespace

// Records one case result, updates the running totals, and writes evidence.
class CaseRecorder
{
public:
    CaseRecorder(CaseLogger& logger, MatrixSummary& summary)
        : m_logger(logger), m_summary(summary)
    {
    }

    void Record(
        const std::string& caseId,
        const std::string& phase,
        const std::string& result,
        const std::string& expectedJson,
        const std::string& actualJson,
        const std::vector<std::string>& errors)
    {
        ++m_index;
        if (result == "pass") ++m_summary.passed;
        else if (result == "fail") ++m_summary.failed;
        else if (result == "skip") ++m_summary.skipped;
        else ++m_summary.errored;
        m_logger.WriteCase(m_index, caseId, phase, result, expectedJson, actualJson, errors);
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
    std::vector<std::string> errors;
    if (!ok)
    {
        errors.push_back("transport_selection_mismatch");
    }
    recorder.Record(
        row.id,
        soft ? "selection_soft" : "selection_hard",
        ok ? "pass" : "fail",
        ExpectJson(row.expectOk, row.expectTransport),
        ChoiceJson(choice),
        errors);
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
    const std::string& workflowJson,
    bool expectReady)
{
    notch_comfy::HttpResponse response;
    std::string sendError;
    if (!http.Send(ClientProtocol::BuildRequiredFilesRequest(workflowJson), response, sendError))
    {
        recorder.Record(caseId, "readiness", "error", "null",
            "{\"error\":" + CaseLogger::Quote(sendError) + "}",
            std::vector<std::string>{"harness_error"});
        return;
    }
    std::vector<notch_comfy::RequiredFile> files;
    std::string parseError;
    if (!ClientProtocol::ParseRequiredFilesResponse(response.m_body, files, parseError))
    {
        recorder.Record(caseId, "readiness", "error", "null",
            "{\"error\":" + CaseLogger::Quote(parseError) + "}",
            std::vector<std::string>{"required_files_parse_error"});
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
    std::vector<std::string> errors;
    if (!ok)
    {
        errors.push_back("deployment_readiness_mismatch");
    }
    std::ostringstream actual;
    actual << "{\"ready\":" << CaseLogger::Bool(ready)
           << ",\"missing_files\":" << missing
           << ",\"client_would_block\":" << CaseLogger::Bool(!ready) << "}";
    recorder.Record(
        caseId, "readiness", ok ? "pass" : "fail",
        "{\"ready\":" + std::string(expectReady ? "true" : "false") + "}",
        actual.str(), errors);
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
    stream << json.str() << "\n";
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
        std::vector<std::string> errors;
        if (!compat.m_ok)
        {
            errors.push_back("server_feature_mismatch");
        }
        std::ostringstream actual;
        actual << "{\"ok\":" << CaseLogger::Bool(compat.m_ok)
               << ",\"client_too_old\":" << CaseLogger::Bool(compat.m_clientTooOld)
               << ",\"server_too_old\":" << CaseLogger::Bool(compat.m_serverTooOld) << "}";
        recorder.Record(
            "server-wire-compatibility", "discovery",
            compat.m_ok ? "pass" : "fail",
            "{\"ok\":true}", actual.str(), errors);
    }

    // 4. Live /notch/parse type-axis assertions, when a workflow fixture exists.
    if (options.parseWorkflowJson.empty())
    {
        recorder.Record(
            "parse-type-axis", "discovery", "skip",
            "null", "{\"reason\":\"no_parse_workflow\"}",
            std::vector<std::string>());
    }
    else
    {
        notch_comfy::HttpResponse parseResponse;
        std::string sendError;
        if (!http.Send(ClientProtocol::BuildWorkflowDiscoveryRequest(options.parseWorkflowJson), parseResponse, sendError))
        {
            recorder.Record(
                "parse-type-axis", "discovery", "error",
                "null", "{\"error\":" + CaseLogger::Quote(sendError) + "}",
                std::vector<std::string>{"harness_error"});
        }
        else
        {
            notch_comfy::WorkflowContract contract;
            std::string contractError;
            if (!ClientProtocol::ParseWorkflowContract(parseResponse.m_body, contract, contractError))
            {
                recorder.Record(
                    "parse-type-axis", "discovery", "error",
                    "null", "{\"error\":" + CaseLogger::Quote(contractError) + "}",
                    std::vector<std::string>{"parse_contract_mismatch"});
            }
            else
            {
                for (size_t i = 0; i < contract.m_outputs.size(); ++i)
                {
                    const notch_comfy::ContractOutput& output = contract.m_outputs[i];
                    std::vector<std::string> expected = ExpectedTypeTransports(output.m_type);
                    bool ok = Sorted(expected) == Sorted(output.m_transports);
                    std::vector<std::string> errors;
                    if (!ok)
                    {
                        errors.push_back("parse_contract_mismatch");
                    }
                    recorder.Record(
                        "parse-output-" + output.m_name,
                        "discovery",
                        ok ? "pass" : "fail",
                        "{\"type\":" + CaseLogger::Quote(output.m_type) + ",\"transports\":" + CaseLogger::Array(expected) + "}",
                        "{\"type\":" + CaseLogger::Quote(output.m_type) + ",\"transports\":" + CaseLogger::Array(output.m_transports) + "}",
                        errors);
                }
            }
        }
    }

    // 4b. Deployment-readiness gate (required input files reachable on server).
    if (options.requiredFilesReadyJson.empty() && options.requiredFilesMissingJson.empty())
    {
        recorder.Record(
            "deployment-readiness", "readiness", "skip", "null",
            "{\"reason\":\"no_required_files_fixture\"}",
            std::vector<std::string>());
    }
    else
    {
        if (!options.requiredFilesReadyJson.empty())
        {
            RunReadinessCase(recorder, http, "deployment-ready", options.requiredFilesReadyJson, true);
        }
        if (!options.requiredFilesMissingJson.empty())
        {
            RunReadinessCase(recorder, http, "deployment-missing-file", options.requiredFilesMissingJson, false);
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
            recorder.Record(
                "ws-handshake-smoke", "discovery", "fail",
                "{\"connected\":true}",
                "{\"connected\":false,\"error\":" + CaseLogger::Quote(wsError) + "}",
                std::vector<std::string>{"websocket_timeout"});
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
            std::vector<std::string> errors;
            if (!sent)
            {
                errors.push_back("websocket_timeout");
            }
            recorder.Record(
                "ws-handshake-smoke", "discovery",
                sent ? "pass" : "fail",
                "{\"connected\":true,\"sent\":true}", actual.str(), errors);
        }
    }

    WriteResultFile(options.outputRoot, summary);
    return summary;
}

} // namespace notch_mock
