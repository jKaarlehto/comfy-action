#ifndef NOTCH_MOCK_FACADE_LOG_H
#define NOTCH_MOCK_FACADE_LOG_H

#include <string>

#include "case_logger.h"

namespace notch_mock
{

// Structured, semantic trace of what the ComfyExtensionClient::Client facade is
// doing: one record per facade action (discover, connect, parse_workflow,
// select_output_transport, submit, event, fetch_output, assert). Each record is
// streamed as a JSON line to mock-client.jsonl via CaseLogger::Event, and a
// human-readable one-line summary is accumulated so a delivery case can attach a
// skimmable facade-trace.log alongside the raw http.jsonl / websocket.jsonl wire
// logs. This is the "what's happening" layer: it shows the facade's decisions
// (negotiation, validation, correlation), not just the bytes on the wire.
class FacadeLog
{
public:
    FacadeLog(CaseLogger& logger, const std::string& phase);

    // Scope subsequent actions to a case id and reset the readable accumulator.
    void SetCase(const std::string& caseId);

    // Emit one facade action. fieldsJson is a compact JSON object body WITHOUT
    // the enclosing braces (e.g. "\"prompt_id\":\"p1\",\"queued\":true"), or "".
    // result is "ok" | "fail" | "skip"; error is optional.
    void Action(const std::string& action,
                const std::string& fieldsJson,
                const std::string& result,
                const std::string& error = "");

    // Human-readable one-line-per-action trace for the current case.
    const std::string& Lines() const { return m_lines; }

private:
    CaseLogger& m_logger;
    std::string m_phase;
    std::string m_caseId;
    std::string m_lines;
};

} // namespace notch_mock

#endif
