#include "facade_log.h"

#include <sstream>

namespace notch_mock
{

FacadeLog::FacadeLog(CaseLogger& logger, const std::string& phase)
    : m_logger(logger), m_phase(phase)
{
}

void FacadeLog::SetCase(const std::string& caseId)
{
    m_caseId = caseId;
    m_lines.clear();
}

void FacadeLog::Action(const std::string& action,
                       const std::string& fieldsJson,
                       const std::string& result,
                       const std::string& error)
{
    std::ostringstream record;
    record << "{\"record\":\"facade_action\""
           << ",\"phase\":" << CaseLogger::Quote(m_phase)
           << ",\"case\":" << CaseLogger::Quote(m_caseId)
           << ",\"action\":" << CaseLogger::Quote(action);
    if (!fieldsJson.empty())
    {
        record << "," << fieldsJson;
    }
    record << ",\"result\":" << CaseLogger::Quote(result);
    if (!error.empty())
    {
        record << ",\"error\":" << CaseLogger::Quote(error);
    }
    record << "}";
    m_logger.Event(record.str());

    std::ostringstream line;
    line << "facade " << action << " -> " << result;
    if (!fieldsJson.empty())
    {
        line << " {" << fieldsJson << "}";
    }
    if (!error.empty())
    {
        line << " error=" << error;
    }
    line << "\n";
    m_lines += line.str();
}

} // namespace notch_mock
