#ifndef NOTCH_MOCK_LOCAL_WEBSOCKET_TRANSPORT_H
#define NOTCH_MOCK_LOCAL_WEBSOCKET_TRANSPORT_H

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>

#include "matrix.h"

namespace notch_mock
{

// IWebSocketProbe (and the interface's IWebSocketTransport, via SendText)
// backed by ix::WebSocket. Connects to /ws?clientId=<id>, sends text, and
// records received text frames to <outputRoot>/conformance/websocket.jsonl.
class LocalWebSocketTransport : public IWebSocketProbe,
                                public notch_comfy::IWebSocketTransport
{
public:
    LocalWebSocketTransport(const std::string& url, const std::string& outputRoot);
    ~LocalWebSocketTransport() override;

    bool Connect(int timeoutMs, std::string& error) override;
    bool SendText(const std::string& message, std::string& error) override;
    std::vector<std::string> DrainReceived() override;
    void Close() override;

private:
    ix::WebSocket m_ws;
    std::string m_outputRoot;
    std::mutex m_mutex;
    std::vector<std::string> m_received;
    std::atomic<bool> m_open;
    std::string m_lastError;
};

} // namespace notch_mock

#endif
