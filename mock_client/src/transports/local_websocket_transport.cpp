#include "transports/local_websocket_transport.h"

#include <chrono>
#include <fstream>
#include <thread>

#include "case_logger.h"

namespace notch_mock
{

LocalWebSocketTransport::LocalWebSocketTransport(const std::string& url, const std::string& outputRoot)
    : m_outputRoot(outputRoot), m_open(false)
{
    m_ws.setUrl(url);
    m_ws.disableAutomaticReconnection();
    m_ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
        switch (message->type)
        {
        case ix::WebSocketMessageType::Open:
            m_open = true;
            break;
        case ix::WebSocketMessageType::Close:
        case ix::WebSocketMessageType::Error:
            m_open = false;
            if (message->type == ix::WebSocketMessageType::Error)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastError = message->errorInfo.reason;
            }
            break;
        case ix::WebSocketMessageType::Message:
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_received.push_back(message->str);
            std::ofstream stream(m_outputRoot + "/conformance/websocket.jsonl", std::ios::app);
            stream << "{\"direction\":\"recv\",\"bytes\":" << message->str.size() << "}\n";
            break;
        }
        default:
            break;
        }
    });
}

LocalWebSocketTransport::~LocalWebSocketTransport()
{
    Close();
}

bool LocalWebSocketTransport::Connect(int timeoutMs, std::string& error)
{
    m_ws.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!m_open && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!m_open)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        error = m_lastError.empty() ? "websocket did not open before timeout" : m_lastError;
        return false;
    }
    return true;
}

bool LocalWebSocketTransport::SendText(const std::string& message, std::string& error)
{
    if (!m_open)
    {
        error = "websocket is not open";
        return false;
    }
    ix::WebSocketSendInfo info = m_ws.send(message);
    {
        std::ofstream stream(m_outputRoot + "/conformance/websocket.jsonl", std::ios::app);
        stream << "{\"direction\":\"send\",\"bytes\":" << message.size()
               << ",\"success\":" << CaseLogger::Bool(info.success) << "}\n";
    }
    if (!info.success)
    {
        error = "websocket send failed";
        return false;
    }
    // Give the server a brief window to deliver catch-up frames.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return true;
}

std::vector<std::string> LocalWebSocketTransport::DrainReceived()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> drained;
    drained.swap(m_received);
    return drained;
}

void LocalWebSocketTransport::Close()
{
    m_ws.stop();
    m_open = false;
}

} // namespace notch_mock
