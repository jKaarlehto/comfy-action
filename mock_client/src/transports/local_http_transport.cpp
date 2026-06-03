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

bool LocalHttpTransport::Send(const notch_comfy::HttpRequest& request,
                              notch_comfy::HttpResponse& response,
                              std::string& error)
{
    ix::HttpClient client(/*async=*/false);
    const std::string url = m_baseUrl + request.m_path;

    ix::HttpRequestArgsPtr args = client.createRequest(url, request.m_method);
    if (!request.m_contentType.empty())
    {
        args->extraHeaders["Content-Type"] = request.m_contentType;
    }

    ix::HttpResponsePtr res = client.request(url, request.m_method, request.m_body, args);

    bool ok = true;
    if (!res)
    {
        error = "no http response";
        ok = false;
    }
    else
    {
        response.m_statusCode = res->statusCode;
        response.m_body = res->body;
        if (res->statusCode <= 0)
        {
            error = res->errorMsg.empty() ? "http transport error" : res->errorMsg;
            ok = false;
        }
    }

    std::ostringstream record;
    record << "{\"method\":" << CaseLogger::Quote(request.m_method)
           << ",\"path\":" << CaseLogger::Quote(request.m_path)
           << ",\"status\":" << (res ? res->statusCode : -1)
           << ",\"request_bytes\":" << request.m_body.size()
           << ",\"response_bytes\":" << response.m_body.size()
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
