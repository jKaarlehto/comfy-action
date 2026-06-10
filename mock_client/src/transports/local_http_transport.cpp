#include "transports/local_http_transport.h"

#include <fstream>
#include <sstream>

#include <ixwebsocket/IXHttpClient.h>

#include "case_logger.h"

namespace notch_mock
{

LocalHttpTransport::LocalHttpTransport(const std::string& baseUrl, const std::string& outputRoot)
    : m_baseUrl(baseUrl), m_outputRoot(outputRoot)
{
}

bool LocalHttpTransport::Send(const ComfyExtensionClient::HttpRequest& request,
                              ComfyExtensionClient::HttpResponse& response,
                              std::string& error)
{
    m_lastRequest = request;

    ix::HttpClient client(/*async=*/false);
    const std::string url = m_baseUrl + request.path;

    ix::HttpRequestArgsPtr args = client.createRequest(url, request.method);
    if (!request.content_type.empty())
    {
        args->extraHeaders["Content-Type"] = request.content_type;
    }

    ix::HttpResponsePtr res = client.request(url, request.method, request.body, args);

    bool ok = true;
    if (!res)
    {
        error = "no http response";
        ok = false;
    }
    else
    {
        response.status_code = res->statusCode;
        response.body = res->body;
        if (res->statusCode <= 0)
        {
            error = res->errorMsg.empty() ? "http transport error" : res->errorMsg;
            ok = false;
        }
    }

    std::ostringstream record;
    record << "{\"method\":" << CaseLogger::Quote(request.method)
           << ",\"path\":" << CaseLogger::Quote(request.path)
           << ",\"status\":" << (res ? res->statusCode : -1)
           << ",\"request_bytes\":" << request.body.size()
           << ",\"response_bytes\":" << response.body.size()
           << ",\"ok\":" << CaseLogger::Bool(ok);
    if (!ok)
    {
        record << ",\"error\":" << CaseLogger::Quote(error);
    }
    record << "}";

    std::ofstream stream(m_outputRoot + "/conformance/http.jsonl", std::ios::app);
    stream << record.str() << "\n";

    return ok;
}

} // namespace notch_mock
