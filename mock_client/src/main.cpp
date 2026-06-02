// notch_mock_client: the CI consumer of cpp/notch_comfy_client. It wires the
// IXWebSocket-backed transports into the transport-agnostic matrix orchestration
// and runs the matrix v1 discovery + selection + handshake-smoke suite.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "case_logger.h"
#include "matrix.h"
#include "transports/local_http_transport.h"
#include "transports/local_websocket_transport.h"

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
    const std::string clientId = ArgValue(argc, argv, "--client-id", "notch-contract-ci");
    const std::string parseWorkflowPath = ArgValue(argc, argv, "--parse-workflow", "");
    const std::string requiredFilesReadyPath = ArgValue(argc, argv, "--required-files-ready", "");
    const std::string requiredFilesMissingPath = ArgValue(argc, argv, "--required-files-missing", "");
    const std::string wsUrl =
        ArgValue(argc, argv, "--ws-url", DeriveWsBase(baseUrl) + "/ws?clientId=" + clientId);

    notch_mock::CaseLogger logger(outputRoot);
    notch_mock::LocalHttpTransport http(baseUrl, outputRoot);
    notch_mock::LocalWebSocketTransport ws(wsUrl, outputRoot);

    notch_mock::MatrixOptions options;
    options.baseUrl = baseUrl;
    options.outputRoot = outputRoot;
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

    notch_mock::MatrixSummary summary = notch_mock::RunMatrixV1(http, ws, options, logger);

    std::cout << "contract matrix v1: pass=" << summary.passed
              << " fail=" << summary.failed
              << " skip=" << summary.skipped
              << " error=" << summary.errored << std::endl;
    if (!summary.setupFailureCode.empty())
    {
        std::cout << "setup failure: " << summary.setupFailureCode << std::endl;
    }

    return summary.Ok() ? 0 : 1;
}
