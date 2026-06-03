// Transport-agnostic self-test for the conformance orchestration. It drives
// RunConformance with stub transports (no IXWebSocket, no live server) so the
// contract logic can be compiled and exercised without network access. The
// real adapters in src/transports are checked separately by the Docker build.

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "case_logger.h"
#include "delivery_types.h"
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
        // injected prompt_id; emit it (also serves as a catch-up frame for the handshake check).
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
    options.phase = notch_mock::Phase::Negotiation;
    options.baseUrl = "http://127.0.0.1:8188";
    options.outputRoot = "stub-out";
    options.clientId = "stub-client";
    options.parseWorkflowJson = "{\"image_output\":1}";
    options.requiredFilesReadyJson = "{\"all_present\":1}";
    options.requiredFilesMissingJson = "{\"missing_file\":1}";

    notch_mock::MatrixSummary summary = notch_mock::RunConformance(http, ws, options, logger);

    std::printf("stub conformance: pass=%d fail=%d skip=%d error=%d\n",
                summary.passed, summary.failed, summary.skipped, summary.errored);

    // negotiation: 1 wire-compat + 2 type-axis + 2 readiness + 10 hard + 4 soft + 1 ws = 20.
    if (!summary.Ok() || summary.passed != 20 || summary.failed != 0)
    {
        std::printf("stub conformance self-test FAILED\n");
        return 1;
    }
    std::printf("stub conformance self-test OK\n");

    // Delivery type-table cardinalities (spec §4): pure set arithmetic, no server.
    {
        const std::vector<std::string> fullServer = {"cuda", "disk", "http"};

        // local topology: client reaches everything -> image x3 + 6 non-image x2 = 15
        // positives, and 0 reachability rejects (every type-allowed transport is reachable).
        int localPositives = 0;
        int localReachabilityRejects = 0;
        for (const auto& type : notch_mock::DeliveryTypes())
        {
            const auto usable = notch_mock::UsableTransports(type, fullServer, fullServer);
            localPositives += static_cast<int>(usable.size());
            localReachabilityRejects +=
                static_cast<int>(notch_mock::TypeAllowedTransports(type).size() - usable.size());
        }
        assert(localPositives == 15);
        assert(localReachabilityRejects == 0);

        // remote-http topology: client reaches only http -> 7 positives (one per type),
        // and 8 reachability rejects (image: cuda+disk unreachable = 2; each of 6
        // non-image: disk unreachable = 1).
        const std::vector<std::string> httpOnlyClient = {"http"};
        int remotePositives = 0;
        int remoteReachabilityRejects = 0;
        for (const auto& type : notch_mock::DeliveryTypes())
        {
            const auto usable = notch_mock::UsableTransports(type, fullServer, httpOnlyClient);
            remotePositives += static_cast<int>(usable.size());
            remoteReachabilityRejects +=
                static_cast<int>(notch_mock::TypeAllowedTransports(type).size() - usable.size());
        }
        assert(remotePositives == 7);
        assert(remoteReachabilityRejects == 8);

        // remote-route-disk topology: a matching server/client named route makes
        // disk reachable again, but cuda remains host-local and unreachable.
        const std::vector<std::string> routeDiskClient = {"disk", "http"};
        int remoteRoutePositives = 0;
        int remoteRouteRejects = 0;
        for (const auto& type : notch_mock::DeliveryTypes())
        {
            const auto usable = notch_mock::UsableTransports(type, fullServer, routeDiskClient);
            remoteRoutePositives += static_cast<int>(usable.size());
            remoteRouteRejects +=
                static_cast<int>(notch_mock::TypeAllowedTransports(type).size() - usable.size());
        }
        assert(remoteRoutePositives == 14);
        assert(remoteRouteRejects == 1);
    }
    std::printf("delivery type-table cardinality assertions OK\n");
    return 0;
}
