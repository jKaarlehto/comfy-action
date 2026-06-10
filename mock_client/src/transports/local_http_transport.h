#ifndef NOTCH_MOCK_LOCAL_HTTP_TRANSPORT_H
#define NOTCH_MOCK_LOCAL_HTTP_TRANSPORT_H

#include <string>

#include "comfy_extension_client/client.hpp"

namespace notch_mock
{

// ComfyExtensionClient::HttpTransport backed by ix::HttpClient. Talks plain
// http:// to the local ComfyUI server and appends a sanitized record per request
// to <outputRoot>/conformance/http.jsonl for CI evidence. This is the transport
// the Client facade drives: the facade builds every request and hands it here.
//
// The last request the facade asked us to send is retained so a delivery case
// can attach the (facade-built) inject body as evidence without re-building it.
class LocalHttpTransport : public ComfyExtensionClient::HttpTransport
{
public:
    LocalHttpTransport(const std::string& baseUrl, const std::string& outputRoot);

    bool Send(const ComfyExtensionClient::HttpRequest& request,
              ComfyExtensionClient::HttpResponse& response,
              std::string& error) override;

    // The most recent request the facade sent through this transport. After a
    // Client::Submit() this is the inject POST; used only for evidence capture.
    const ComfyExtensionClient::HttpRequest& LastRequest() const { return m_lastRequest; }

private:
    std::string m_baseUrl;
    std::string m_outputRoot;
    ComfyExtensionClient::HttpRequest m_lastRequest;
};

} // namespace notch_mock

#endif
