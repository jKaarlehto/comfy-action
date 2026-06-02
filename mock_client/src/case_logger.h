#ifndef NOTCH_MOCK_CASE_LOGGER_H
#define NOTCH_MOCK_CASE_LOGGER_H

#include <string>
#include <vector>

namespace notch_mock
{

// One contract-matrix case result, carrying enough self-description to read the
// artifact without the source: a plain-language title, the phase it belongs to,
// why it exists, a pointer to the spec it is derived from, and what was
// requested.
struct CaseRecord
{
    int index = 0;
    std::string caseId;
    std::string title;        // plain one-line summary of what the case checks
    std::string phase;        // liveness | discovery | readiness | transport-selection
    std::string description;  // why this case exists, in prose
    std::string specRef;      // pointer into docs/notch_contract_matrix_spec.md
    // Transport-selection cases set exactly one of these to describe the request:
    std::string requestedTransport;          // single required transport (never downgraded)
    std::vector<std::string> preferredOrder; // ordered preference (first usable wins)
    std::string expectedJson;                // compact JSON fragment, or empty -> null
    std::string actualJson;                  // compact JSON fragment, or empty -> null
    std::string result;                      // pass | fail | skip | error
    std::vector<std::string> errors;
};

// CaseLogger writes the contract-matrix evidence tree under a single output
// root: mock-client.jsonl for run-wide streaming records, one prettified
// case.json per case, and an INDEX.md human report. The .json files are
// pretty-printed; the .jsonl streaming log stays one record per line (the JSON
// Lines contract — read line by line, not pretty-printed).
class CaseLogger
{
public:
    explicit CaseLogger(const std::string& outputRoot);

    // Append one already-formed JSON object as a line to mock-client.jsonl.
    void Event(const std::string& jsonObject);

    // Write cases/<NNN>-<caseId>/case.json (prettified) and remember it for INDEX.md.
    void WriteCase(const CaseRecord& record);

    // Write INDEX.md: every recorded case grouped by phase with title + result.
    void WriteIndex();

    // JSON-escape a string and wrap it in double quotes.
    static std::string Quote(const std::string& value);
    static std::string Bool(bool value);
    static std::string Array(const std::vector<std::string>& values);
    // Reformat a compact JSON string with two-space indentation.
    static std::string PrettyPrint(const std::string& compactJson);

private:
    std::string m_root;
    std::vector<CaseRecord> m_records;
    std::string MatrixDir() const;
};

} // namespace notch_mock

#endif
