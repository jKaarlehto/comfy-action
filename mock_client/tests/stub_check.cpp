// Transport-agnostic self-test for the conformance orchestration. It drives
// RunConformance with stub transports (no IXWebSocket, no live server) so the
// contract logic can be compiled and exercised without network access. The
// real adapters in src/transports are checked separately by the Docker build.

#include <cstdio>
#include <string>
#include <vector>

#include "case_logger.h"
#include "matrix.h"

namespace
{

class StubHttp : public notch_comfy::IHttpTransport
{
public:
    bool Send(const notch_comfy::HttpRequest& request,
              notch_comfy::HttpResponse& response,
              std::string& error) override
    {
        const std::string& path = request.m_path;
        if (path == "/features")
        {
            response.m_statusCode = 200;
            response.m_body =
                "{\"extension\":{\"notch\":{"
                "\"protocol_version\":1,"
                "\"minimum_client_protocol_version\":1,"
                "\"cuda_device_index\":-1,"
                "\"output_transports\":[\"disk\",\"http\",\"noop\"]}}}";
            return true;
        }
        if (path == "/notch/parse")
        {
            // Both type-axis equivalence classes: image (cuda,disk,http) and a
            // non-image FILE_3D output (disk,http).
            response.m_statusCode = 200;
            response.m_body =
                "{\"inputs\":[],\"schema\":{},\"outputs\":["
                "{\"name\":\"image\",\"type\":\"IMAGE\",\"transports\":[\"cuda\",\"disk\",\"http\"]},"
                "{\"name\":\"mesh\",\"type\":\"FILE_3D_GLB\",\"transports\":[\"disk\",\"http\"]}]}";
            return true;
        }
        if (path == "/notch/get-required-files")
        {
            response.m_statusCode = 200;
            // The "missing" fixture references a file the server cannot reach.
            if (request.m_body.find("missing") != std::string::npos)
            {
                response.m_body =
                    "{\"files\":[{\"filename\":\"absent.safetensors\",\"category\":\"checkpoints\","
                    "\"full_path\":\"/models/checkpoints/absent.safetensors\",\"exists\":false}]}";
            }
            else
            {
                response.m_body =
                    "{\"files\":[{\"filename\":\"present.safetensors\",\"category\":\"checkpoints\","
                    "\"full_path\":\"/models/checkpoints/present.safetensors\",\"exists\":true}]}";
            }
            return true;
        }
        if (path.find("/notch/inject") != std::string::npos)
        {
            response.m_statusCode = 200;
            // A workflow referencing a missing file is rejected at inject (blocked).
            if (request.m_body.find("missing_ckpt") != std::string::npos)
            {
                response.m_body = "{\"error\":\"required input file is missing\"}";
            }
            else
            {
                response.m_body = "{\"queued\":true,\"prompt_id\":\"psuccess\"}";
            }
            return true;
        }
        if (path.find("/notch/diagnostics") != std::string::npos)
        {
            response.m_statusCode = 200;
            response.m_body = "{\"ci_enabled\":true,\"prompt_id\":\"psuccess\",\"count\":0,\"records\":[]}";
            return true;
        }
        error = "unexpected path " + path;
        return false;
    }
};

class StubProbe : public notch_mock::IWebSocketProbe
{
public:
    bool Connect(int, std::string&) override { return true; }
    bool SendText(const std::string&, std::string&) override { return true; }
    std::vector<std::string> DrainReceived() override
    {
        // The execution-success case waits for a terminal event matching the
        // injected prompt_id; emit it (also serves as a catch-up frame for smoke).
        return std::vector<std::string>{
            "{\"type\":\"execution_success\",\"data\":{\"prompt_id\":\"psuccess\"}}"};
    }
    void Close() override {}
};

} // namespace

int main()
{
    StubHttp http;
    StubProbe ws;
    notch_mock::CaseLogger logger("stub-out");

    notch_mock::MatrixOptions options;
    options.phase = notch_mock::Phase::All;
    options.baseUrl = "http://127.0.0.1:8188";
    options.outputRoot = "stub-out";
    options.clientId = "stub-client";
    options.parseWorkflowJson = "{\"image_output\":1}";
    options.requiredFilesReadyJson = "{\"all_present\":1}";
    options.requiredFilesMissingJson = "{\"missing_file\":1}";
    options.executeWorkflowJson = "{\"empty_image\":1}";
    options.executeMissingFileJson = "{\"missing_ckpt\":1}";

    notch_mock::MatrixSummary summary = notch_mock::RunConformance(http, ws, options, logger);

    std::printf("stub conformance: pass=%d fail=%d skip=%d error=%d\n",
                summary.passed, summary.failed, summary.skipped, summary.errored);

    // negotiation: 1 wire-compat + 2 type-axis + 2 readiness + 10 hard + 3 soft + 1 ws = 19.
    // delivery: 1 execution-success + 1 file-availability-blocked = 2.  Total = 21.
    if (!summary.Ok() || summary.passed != 21 || summary.failed != 0)
    {
        std::printf("stub conformance self-test FAILED\n");
        return 1;
    }
    std::printf("stub conformance self-test OK\n");
    return 0;
}
