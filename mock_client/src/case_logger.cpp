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
    EnsureDir(ConformanceDir() + "/cases");
}

std::string CaseLogger::ConformanceDir() const
{
    return m_root + "/conformance";
}

std::string CaseLogger::CaseDir(int index, const std::string& caseId) const
{
    return ConformanceDir() + "/cases/" + Pad3(index) + "-" + caseId;
}

void CaseLogger::Event(const std::string& jsonObject)
{
    std::ofstream stream(ConformanceDir() + "/mock-client.jsonl", std::ios::app);
    stream << jsonObject << "\n";
}

void CaseLogger::AppendCaseFile(int index, const std::string& caseId, const std::string& filename,
                                const std::string& content)
{
    const std::string dir = CaseDir(index, caseId);
    EnsureDir(dir);
    std::ofstream stream(dir + "/" + filename, std::ios::app);
    stream << content;
}

void CaseLogger::WriteCase(const CaseRecord& record)
{
    const std::string dir = CaseDir(record.index, record.caseId);
    EnsureDir(dir);

    std::ostringstream json;
    json << "{"
         << "\"case_id\":" << Quote(record.caseId) << ","
         << "\"title\":" << Quote(record.title) << ","
         << "\"phase\":" << Quote(record.phase) << ","
         << "\"description\":" << Quote(record.description) << ","
         << "\"spec_ref\":" << Quote(record.specRef) << ",";
    if (!record.requiredTransport.empty())
    {
        json << "\"required_transport\":" << Quote(record.requiredTransport) << ",";
    }
    if (!record.preferredOrder.empty())
    {
        json << "\"preferred_order\":" << Array(record.preferredOrder) << ",";
    }
    if (!record.fixtureJson.empty())
    {
        json << "\"fixture\":" << record.fixtureJson << ",";
    }
    json << "\"expected\":" << (record.expectedJson.empty() ? "null" : record.expectedJson) << ","
         << "\"actual\":" << (record.actualJson.empty() ? "null" : record.actualJson) << ","
         << "\"result\":" << Quote(record.result) << ","
         << "\"errors\":" << Array(record.errors)
         << "}";

    const std::string compact = json.str();

    // case.json is prettified for human reading; the streaming log stays JSONL.
    std::ofstream stream(dir + "/case.json");
    stream << PrettyPrint(compact) << "\n";

    Event(compact);
    m_records.push_back(record);
}

void CaseLogger::WriteIndex()
{
    static const char* kPhases[] = {"liveness", "discovery", "readiness", "transport-selection",
                                    "delivery-local", "delivery-remote"};

    std::ostringstream md;
    md << "# Conformance — case index\n\n"
       << "Each `cases/NNN-<id>/case.json` is a self-describing result: `title` (what\n"
       << "it checks), `description` (why), `spec_ref` (where it is derived), the\n"
       << "request, and `expected` vs `actual`. The cases are the output of the axis\n"
       << "analysis in `docs/notch_conformance_spec.md` (§2 axes, §3 counting\n"
       << "criterion, §4 exact counts) — not ad hoc.\n\n"
       << "**Who owns what (transport-selection cases).** A transport is *usable* only\n"
       << "if it is allowed by the output's type (Comfy-owned), available on the server\n"
       << "(server/GPU), and reachable by the client (the Notch client's deployment).\n"
       << "The **client** computes that usable set and **requests** one transport (or a\n"
       << "preference order); the server honors it only if usable, otherwise it\n"
       << "hard-errors — never a silent downgrade. So \"client requests cuda → rejected\"\n"
       << "means the client asked for cuda but cuda was ruled out by type, server, or\n"
       << "reachability. \"Non-image output\" means the output type is not IMAGE (a mesh,\n"
       << "audio, or file), which matters because cuda is image-only.\n\n"
       << "Grouped by phase:\n";

    for (const char* phase : kPhases)
    {
        bool header = false;
        for (const CaseRecord& r : m_records)
        {
            if (r.phase != phase)
            {
                continue;
            }
            if (!header)
            {
                md << "\n## " << phase << "\n\n";
                header = true;
            }
            md << "- `" << Pad3(r.index) << "-" << r.caseId << "` — " << r.title
               << "  _(" << r.result << ")_\n";
        }
    }

    std::ofstream stream(ConformanceDir() + "/INDEX.md");
    stream << md.str();
}

std::string CaseLogger::PrettyPrint(const std::string& compactJson)
{
    // Defensive: this only formats JSON, but it may be handed non-JSON (e.g. a
    // multipart request body). Clamp the indent to [0, kMaxIndent] so unbalanced
    // or binary input can never drive string::append() to a huge/overflowing size
    // (which throws std::length_error and terminates the process).
    auto pad = [](int level) -> std::string {
        const int kMaxIndent = 64;
        if (level < 0) level = 0;
        if (level > kMaxIndent) level = kMaxIndent;
        return std::string(static_cast<size_t>(level) * 2, ' ');
    };
    std::string out;
    int indent = 0;
    bool inString = false;
    bool escape = false;
    for (size_t i = 0; i < compactJson.size(); ++i)
    {
        char c = compactJson[i];
        if (inString)
        {
            out += c;
            if (escape)
            {
                escape = false;
            }
            else if (c == '\\')
            {
                escape = true;
            }
            else if (c == '"')
            {
                inString = false;
            }
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
        {
            continue; // drop insignificant whitespace; indentation is re-added below
        }
        switch (c)
        {
        case '"':
            inString = true;
            out += c;
            break;
        case '{':
        case '[':
            if (i + 1 < compactJson.size() && (compactJson[i + 1] == '}' || compactJson[i + 1] == ']'))
            {
                out += c;
                out += compactJson[i + 1];
                ++i;
            }
            else
            {
                ++indent;
                out += c;
                out += '\n';
                out += pad(indent);
            }
            break;
        case '}':
        case ']':
            if (indent > 0) --indent;
            out += '\n';
            out += pad(indent);
            out += c;
            break;
        case ',':
            out += c;
            out += '\n';
            out += pad(indent);
            break;
        case ':':
            out += ": ";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
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
