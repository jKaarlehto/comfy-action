#ifndef NOTCH_MOCK_LOCAL_HTTP_TRANSPORT_H
#define NOTCH_MOCK_LOCAL_HTTP_TRANSPORT_H

#include <string>

#include "notch_comfy_client/client_interface.h"

namespace notch_mock
{

// IHttpTransport backed by ix::HttpClient. Talks plain http:// to the local
// ComfyUI server and appends a sanitized record per request to
// <outputRoot>/conformance/http.jsonl for CI evidence.
class LocalHttpTransport : public notch_comfy::IHttpTransport
{
public:
    LocalHttpTransport(const std::string& baseUrl, const std::string& outputRoot);

    bool Send(const notch_comfy::HttpRequest& request,
              notch_comfy::HttpResponse& response,
              std::string& error) override;

private:
    std::string m_baseUrl;
    std::string m_outputRoot;
};

} // namespace notch_mock

#endif
