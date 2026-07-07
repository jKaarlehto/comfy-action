#include "delivery_event_sink.h"

#include <sstream>

namespace notch_mock
{

DeliveryEventSink::DeliveryEventSink(DeliveryRunState& state, FacadeLog& log)
    : m_state(state), m_log(log)
{
}

void DeliveryEventSink::Configure(const std::string& promptId, const std::string& consumerId,
                                  const std::string& transport)
{
    m_promptId = promptId;
    m_consumerId = consumerId;
    m_transport = transport;
}

void DeliveryEventSink::OnEvent(const ComfyExtensionClient::ClientEvent& event)
{
    using ComfyExtensionClient::EventKind;

    const bool promptMatches = event.prompt_id.empty() || m_promptId.empty() || event.prompt_id == m_promptId;

    switch (event.kind)
    {
    case EventKind::OutputReady:
    {
        for (size_t i = 0; i < event.outputs.size(); ++i)
        {
            const ComfyExtensionClient::OutputReady& output = event.outputs[i];
            const bool outPromptOk = output.prompt_id.empty() || m_promptId.empty() || output.prompt_id == m_promptId;
            const bool consumerOk = output.consumer_id == m_consumerId || output.name == m_consumerId;
            if (outPromptOk && consumerOk && output.transport == m_transport)
            {
                m_state.output = output;
                m_state.outputReady = true;
                std::ostringstream fields;
                fields << "\"transport\":" << CaseLogger::Quote(output.transport)
                       << ",\"consumer_id\":" << CaseLogger::Quote(output.consumer_id);
                m_log.Action("event.output_ready", fields.str(), "ok");
            }
        }
        break;
    }
    case EventKind::CudaShareStatus:
    {
        for (size_t i = 0; i < event.cuda_shares.size(); ++i)
        {
            const ComfyExtensionClient::CudaShareStatus& share = event.cuda_shares[i];
            if (share.name == m_consumerId || share.notch_consumer_id == m_consumerId)
            {
                m_state.cudaShare = share;
                m_state.cudaStatus = true;
                m_log.Action("event.cuda_share_status",
                             "\"name\":" + CaseLogger::Quote(share.name), "ok");
            }
        }
        break;
    }
    case EventKind::ExecutionSuccess:
        if (promptMatches)
        {
            m_state.terminalType = "execution_success";
            m_state.terminalSuccess = true;
            m_log.Action("event.execution_success", "", "ok");
        }
        break;
    case EventKind::ExecutionError:
    case EventKind::ExecutionInterrupted:
        if (promptMatches)
        {
            m_state.terminalType = event.type;
            m_log.Action("event.execution_terminal", "\"type\":" + CaseLogger::Quote(event.type), "fail");
        }
        break;
    default:
        break;
    }
}

void DeliveryEventSink::OnParseError(const ComfyExtensionClient::Error& error)
{
    // Unhandled / third-party frames are context, never a delivery verdict.
    m_log.Action("event.parse_error", "", "skip", error.message);
}

} // namespace notch_mock
