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

class StubHttp : public ComfyExtensionClient::HttpTransport
{
public:
    bool Send(const ComfyExtensionClient::HttpRequest& request,
              ComfyExtensionClient::HttpResponse& response,
              std::string& error) override
    {
        const std::string& path = request.path;
        if (path == "/features")
        {
            response.status_code = 200;
            response.body =
                "{\"extension\":{\"notch\":{"
                "\"plugin_version\":\"0.3.0\","
                "\"protocol_version\":\"0.3.0\","
                "\"supports_protocol\":\">=0.3.0,<0.4.0\","
                "\"minimum_client_protocol\":\"0.3.0\","
                "\"plugin_capabilities\":[\"http-output\",\"disk-output\",\"named-routes\"],"
                "\"tested_comfyui_refs\":[\"v0.23.0\"],"
                "\"cuda_device_index\":-1,"
                "\"output_transports\":[\"disk\",\"http\",\"noop\"]}}}";
            return true;
        }
        if (path == "/notch/parse")
        {
            // Both type-axis equivalence classes: image (cuda,disk,http) and a
            // non-image FILE_3D output (disk,http).
            response.status_code = 200;
            response.body =
                "{\"inputs\":[],\"schema\":{},\"outputs\":["
                "{\"name\":\"image\",\"type\":\"IMAGE\",\"transports\":[\"cuda\",\"disk\",\"http\"]},"
                "{\"name\":\"mesh\",\"type\":\"FILE_3D_GLB\",\"transports\":[\"disk\",\"http\"]}]}";
            return true;
        }
        if (path == "/notch/get-required-files")
        {
            response.status_code = 200;
            // The "missing" fixture references a file the server cannot reach.
            if (request.body.find("missing") != std::string::npos)
            {
                response.body =
                    "{\"files\":[{\"filename\":\"absent.safetensors\",\"category\":\"checkpoints\","
                    "\"full_path\":\"/models/checkpoints/absent.safetensors\",\"exists\":false}]}";
            }
            else
            {
                response.body =
                    "{\"files\":[{\"filename\":\"present.safetensors\",\"category\":\"checkpoints\","
                    "\"full_path\":\"/models/checkpoints/present.safetensors\",\"exists\":true}]}";
            }
            return true;
        }
        if (path.find("/notch/inject") != std::string::npos)
        {
            response.status_code = 200;
            // A workflow referencing a missing file is rejected at inject (blocked).
            if (request.body.find("missing_ckpt") != std::string::npos)
            {
                response.body = "{\"error\":\"required input file is missing\"}";
            }
            else
            {
                response.body = "{\"queued\":true,\"prompt_id\":\"psuccess\"}";
            }
            return true;
        }
        if (path.find("/notch/diagnostics") != std::string::npos)
        {
            response.status_code = 200;
            response.body = "{\"ci_enabled\":true,\"prompt_id\":\"psuccess\",\"count\":0,\"records\":[]}";
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

    // negotiation: 1 protocol-compat + 2 type-axis + 2 readiness + 10 hard + 4 soft + 1 ws = 20.
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

    // Automatic case generation (spec §4 "the arithmetic is the case set"): the
    // generated descriptors per topology must total local 21, remote-http 15,
    // remote-route-disk 15 — purely from the topology x type x transport loop.
    {
        const std::vector<std::string> full = {"cuda", "disk", "http"};
        const notch_mock::TopologyConfig local{"local", full, full};
        const notch_mock::TopologyConfig remoteHttp{"remote-http", full, {"http"}};
        const notch_mock::TopologyConfig routeDisk{"remote-route-disk", full, {"disk", "http"}};

        const auto localCases = notch_mock::GenerateDeliveryCases(local);
        const auto remoteHttpCases = notch_mock::GenerateDeliveryCases(remoteHttp);
        const auto routeDiskCases = notch_mock::GenerateDeliveryCases(routeDisk);
        assert(localCases.size() == 21);
        assert(remoteHttpCases.size() == 15);
        assert(routeDiskCases.size() == 15);

        // local verdict distribution: 15 deliver positives + 6 type-rejects, no others.
        int deliver = 0, rejectType = 0, other = 0;
        for (const auto& descriptor : localCases)
        {
            if (descriptor.verdict == "deliver")
                ++deliver;
            else if (descriptor.verdict == "reject-type")
                ++rejectType;
            else
                ++other;
        }
        assert(deliver == 15);
        assert(rejectType == 6);
        assert(other == 0);

        // remote-http: 7 deliver + 8 reject-client (no type-rejects re-tested here).
        int rDeliver = 0, rRejectClient = 0, rRejectType = 0;
        for (const auto& descriptor : remoteHttpCases)
        {
            if (descriptor.verdict == "deliver")
                ++rDeliver;
            else if (descriptor.verdict == "reject-client")
                ++rRejectClient;
            else if (descriptor.verdict == "reject-type")
                ++rRejectType;
        }
        assert(rDeliver == 7);
        assert(rRejectClient == 8);
        assert(rRejectType == 0);

        // ids are self-describing and derived: spot-check a few.
        bool sawImageCudaDeliver = false, sawAudioCudaRejectType = false, sawImageDiskRejectClient = false;
        for (const auto& descriptor : localCases)
        {
            if (descriptor.id == "local.image.cuda.deliver") sawImageCudaDeliver = true;
            if (descriptor.id == "local.audio.cuda.reject-type") sawAudioCudaRejectType = true;
        }
        for (const auto& descriptor : remoteHttpCases)
        {
            if (descriptor.id == "remote-http.image.disk.reject-client") sawImageDiskRejectClient = true;
        }
        assert(sawImageCudaDeliver);
        assert(sawAudioCudaRejectType);
        assert(sawImageDiskRejectClient);
    }

    std::printf("delivery type-table cardinality assertions OK\n");
    std::printf("delivery case-generation assertions OK (local=21 remote-http=15 route-disk=15)\n");
    return 0;
}
