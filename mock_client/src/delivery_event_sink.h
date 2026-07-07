#ifndef NOTCH_MOCK_DELIVERY_EVENT_SINK_H
#define NOTCH_MOCK_DELIVERY_EVENT_SINK_H

#include <string>
#include <vector>

#include "comfy_extension_client/client.hpp"

#include "facade_log.h"

namespace notch_mock
{

// Mutable per-case delivery state, populated from the typed events the client
// codec dispatches. The harness drains raw frames and forwards them to
// Client::OnWebSocketText, and the codec parses and calls DeliveryEventSink
// below.
struct DeliveryRunState
{
    bool queued = false;
    std::string promptId;
    std::string injectError;
    std::string terminalType;
    bool terminalSuccess = false;
    bool outputReady = false;
    bool cudaStatus = false;
    ComfyExtensionClient::OutputReady output;
    ComfyExtensionClient::CudaShareStatus cudaShare;
    std::vector<std::string> websocketFrames;
};

// ClientEventSink that records the delivery-relevant events the codec dispatches
// into a DeliveryRunState: the terminal lifecycle event for the case's prompt, the
// matching notch-output-ready, and the matching notch-cuda-share-status. The
// server stamps consumer_id on the wire events, with the output name as the
// legacy fallback key. Match keys (prompt/consumer/transport) are configured per
// case before the wait loop runs.
class DeliveryEventSink : public ComfyExtensionClient::ClientEventSink
{
public:
    DeliveryEventSink(DeliveryRunState& state, FacadeLog& log);

    // Configure the keys the current case matches against.
    void Configure(const std::string& promptId, const std::string& consumerId, const std::string& transport);

    void OnEvent(const ComfyExtensionClient::ClientEvent& event) override;
    void OnParseError(const ComfyExtensionClient::Error& error) override;

private:
    DeliveryRunState& m_state;
    FacadeLog& m_log;
    std::string m_promptId;
    std::string m_consumerId;
    std::string m_transport;
};

} // namespace notch_mock

#endif
