#ifndef NOTCH_MOCK_LOCAL_HTTP_TRANSPORT_H
#define NOTCH_MOCK_LOCAL_HTTP_TRANSPORT_H

#include <string>

#include "comfy_extension_client/client_interface.h"

namespace notch_mock
{

// IHttpTransport backed by ix::HttpClient. Talks plain http:// to the local
// ComfyUI server and appends a sanitized record per request to
// <outputRoot>/conformance/http.jsonl for CI evidence.
class LocalHttpTransport : public ComfyExtensionClientProtocol::IHttpTransport
{
public:
    LocalHttpTransport(const std::string& baseUrl, const std::string& outputRoot);

    bool Send(const ComfyExtensionClientProtocol::HttpRequest& request,
              ComfyExtensionClientProtocol::HttpResponse& response,
              std::string& error) override;

private:
    std::string m_baseUrl;
    std::string m_outputRoot;
};

} // namespace notch_mock

#endif
