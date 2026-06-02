#include "case_logger.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <direct.h>
#define NOTCH_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define NOTCH_MKDIR(path) mkdir(path, 0755)
#endif

namespace notch_mock
{

namespace
{

void EnsureDir(const std::string& path)
{
    // Create each path segment in turn. Existing segments are ignored.
    std::string partial;
    for (size_t i = 0; i < path.size(); ++i)
    {
        char c = path[i];
        if (c == '/' && !partial.empty())
        {
            NOTCH_MKDIR(partial.c_str());
        }
        partial.push_back(c);
    }
    if (!partial.empty())
    {
        NOTCH_MKDIR(partial.c_str());
    }
}

std::string Pad3(int index)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%03d", index);
    return std::string(buffer);
}

} // namespace

CaseLogger::CaseLogger(const std::string& outputRoot)
    : m_root(outputRoot)
{
    EnsureDir(MatrixDir() + "/cases");
}

std::string CaseLogger::MatrixDir() const
{
    return m_root + "/contract-matrix";
}

void CaseLogger::Event(const std::string& jsonObject)
{
    std::ofstream stream(MatrixDir() + "/mock-client.jsonl", std::ios::app);
    stream << jsonObject << "\n";
}

void CaseLogger::WriteCase(
    int index,
    const std::string& caseId,
    const std::string& phase,
    const std::string& result,
    const std::string& expectedJson,
    const std::string& actualJson,
    const std::vector<std::string>& errors)
{
    const std::string dir = MatrixDir() + "/cases/" + Pad3(index) + "-" + caseId;
    EnsureDir(dir);

    std::ostringstream json;
    json << "{"
         << "\"case_id\":" << Quote(caseId) << ","
         << "\"phase\":" << Quote(phase) << ","
         << "\"expected\":" << (expectedJson.empty() ? "null" : expectedJson) << ","
         << "\"actual\":" << (actualJson.empty() ? "null" : actualJson) << ","
         << "\"result\":" << Quote(result) << ","
         << "\"errors\":" << Array(errors)
         << "}";

    std::ofstream stream(dir + "/case.json");
    stream << json.str() << "\n";

    Event(json.str());
}

std::string CaseLogger::Quote(const std::string& value)
{
    std::string out = "\"";
    for (char c : value)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            out.push_back(c);
            break;
        }
    }
    out += "\"";
    return out;
}

std::string CaseLogger::Bool(bool value)
{
    return value ? "true" : "false";
}

std::string CaseLogger::Array(const std::vector<std::string>& values)
{
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
        {
            out += ",";
        }
        out += Quote(values[i]);
    }
    out += "]";
    return out;
}

} // namespace notch_mock
