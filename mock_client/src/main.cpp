// notch_mock_client: the CI consumer of cpp/comfy_extension_client. It wires the
// IXWebSocket-backed transports into the transport-agnostic conformance
// orchestration and runs the selected phase (negotiation, delivery-local, or
// delivery-remote).

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "case_logger.h"
#include "matrix.h"
#include "transports/local_http_transport.h"
#include "transports/local_websocket_transport.h"

#if defined(NOTCH_MOCK_HAS_CUDA_RUNTIME)
#include "transports/local_cuda_share_reader.h"
#endif

namespace
{

std::string ArgValue(int argc, char** argv, const std::string& key, const std::string& fallback)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (key == argv[i])
        {
            return argv[i + 1];
        }
    }
    return fallback;
}

std::string ReadFile(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// http://host:port -> ws://host:port (and https -> wss).
std::string DeriveWsBase(const std::string& httpBase)
{
    if (httpBase.rfind("http", 0) == 0)
    {
        return "ws" + httpBase.substr(4);
    }
    return httpBase;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string baseUrl = ArgValue(argc, argv, "--base-url", "http://127.0.0.1:8188");
    const std::string outputRoot = ArgValue(argc, argv, "--output-dir", "/artifacts");
    const std::string clientId = ArgValue(argc, argv, "--client-id", "notch-conformance");
    const std::string phaseArg = ArgValue(argc, argv, "--phase", "negotiation");
    const std::string parseWorkflowPath = ArgValue(argc, argv, "--parse-workflow", "");
    const std::string requiredFilesReadyPath = ArgValue(argc, argv, "--required-files-ready", "");
    const std::string requiredFilesMissingPath = ArgValue(argc, argv, "--required-files-missing", "");
    const std::string assetRoot = ArgValue(argc, argv, "--asset-root", "/workspace/tests/assets/round_trip");
    const std::string sourceFilePath = ArgValue(argc, argv, "--source-file", "");
    const std::string localOutputPath = ArgValue(argc, argv, "--local-output-path", "/tmp/notch-conformance-output");
    const std::string namedRouteId = ArgValue(argc, argv, "--named-route-id", "");
    const std::string namedRouteClientRoot = ArgValue(argc, argv, "--named-route-client-root", "");
    const std::string namedRouteRelativeDirectory =
        ArgValue(argc, argv, "--named-route-relative-directory", "remote-route");
    const std::string serverLogPath = ArgValue(argc, argv, "--server-log", outputRoot + "/comfyui.log");
    const std::string wsUrl =
        ArgValue(argc, argv, "--ws-url", DeriveWsBase(baseUrl) + "/ws?clientId=" + clientId);

    notch_mock::Phase phase = notch_mock::Phase::Negotiation;
    if (phaseArg == "delivery-local")
    {
        phase = notch_mock::Phase::DeliveryLocal;
    }
    else if (phaseArg == "delivery-remote")
    {
        phase = notch_mock::Phase::DeliveryRemote;
    }
    else if (phaseArg != "negotiation")
    {
        std::cerr << "unsupported --phase '" << phaseArg
                  << "'; expected negotiation, delivery-local, or delivery-remote" << std::endl;
        return 2;
    }

    notch_mock::CaseLogger logger(outputRoot);
    notch_mock::LocalHttpTransport http(baseUrl, outputRoot);
    notch_mock::LocalWebSocketTransport ws(wsUrl, outputRoot);
#if defined(NOTCH_MOCK_HAS_CUDA_RUNTIME)
    notch_mock::LocalCudaShareReader cudaReader;
    notch_mock::ICudaShareReader* cudaReaderPtr = &cudaReader;
#else
    notch_mock::ICudaShareReader* cudaReaderPtr = nullptr;
#endif

    notch_mock::MatrixOptions options;
    options.phase = phase;
    options.baseUrl = baseUrl;
    options.outputRoot = outputRoot;
    options.clientId = clientId;
    if (!parseWorkflowPath.empty())
    {
        options.parseWorkflowJson = ReadFile(parseWorkflowPath);
    }
    if (!requiredFilesReadyPath.empty())
    {
        options.requiredFilesReadyJson = ReadFile(requiredFilesReadyPath);
    }
    if (!requiredFilesMissingPath.empty())
    {
        options.requiredFilesMissingJson = ReadFile(requiredFilesMissingPath);
    }
    options.assetRoot = assetRoot;
    options.sourceFilePath = sourceFilePath;
    options.localOutputPath = localOutputPath;
    options.namedRouteId = namedRouteId;
    options.namedRouteClientRoot = namedRouteClientRoot;
    options.namedRouteRelativeDirectory = namedRouteRelativeDirectory;
    options.serverLogPath = serverLogPath;

    notch_mock::MatrixSummary summary = notch_mock::RunConformance(http, ws, options, logger, cudaReaderPtr);

    std::cout << "conformance [" << phaseArg << "]: pass=" << summary.passed
              << " fail=" << summary.failed
              << " skip=" << summary.skipped
              << " error=" << summary.errored << std::endl;
    if (!summary.setupFailureCode.empty())
    {
        std::cout << "setup failure: " << summary.setupFailureCode << std::endl;
    }

    return summary.Ok() ? 0 : 1;
}
