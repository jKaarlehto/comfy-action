// Transport-agnostic self-test for the matrix orchestration. It drives
// RunMatrixV1 with stub transports (no IXWebSocket, no live server) so the
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
        if (request.m_path == "/features")
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
        if (request.m_path == "/notch/parse")
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
        if (request.m_path == "/notch/get-required-files")
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
        error = "unexpected path " + request.m_path;
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
        return std::vector<std::string>{"{\"type\":\"notch-cuda-share-status\"}"};
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
    options.baseUrl = "http://127.0.0.1:8188";
    options.outputRoot = "stub-out";
    options.parseWorkflowJson = "{\"image_output\":1}";
    options.requiredFilesReadyJson = "{\"all_present\":1}";
    options.requiredFilesMissingJson = "{\"missing_file\":1}";

    notch_mock::MatrixSummary summary = notch_mock::RunMatrixV1(http, ws, options, logger);

    std::printf("stub matrix: pass=%d fail=%d skip=%d error=%d\n",
                summary.passed, summary.failed, summary.skipped, summary.errored);

    // 1 wire-compat + 2 type-axis + 2 readiness + 10 hard + 2 soft + 1 ws = 18.
    if (!summary.Ok() || summary.passed != 18 || summary.failed != 0)
    {
        std::printf("stub matrix self-test FAILED\n");
        return 1;
    }
    std::printf("stub matrix self-test OK\n");
    return 0;
}
