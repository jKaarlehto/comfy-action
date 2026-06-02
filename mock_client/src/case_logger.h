#ifndef NOTCH_MOCK_CASE_LOGGER_H
#define NOTCH_MOCK_CASE_LOGGER_H

#include <string>
#include <vector>

namespace notch_mock
{

// CaseLogger writes the contract-matrix evidence tree under a single output
// root: mock-client.jsonl for run-wide records and one case.json per case. It
// favors readable, append-only JSONL over clever abstractions so a failure is
// debuggable from artifacts without replaying the run.
class CaseLogger
{
public:
    explicit CaseLogger(const std::string& outputRoot);

    // Append one already-formed JSON object as a line to mock-client.jsonl.
    void Event(const std::string& jsonObject);

    // Write cases/<NNN>-<caseId>/case.json with the standard result shape.
    void WriteCase(
        int index,
        const std::string& caseId,
        const std::string& phase,
        const std::string& result,
        const std::string& expectedJson,
        const std::string& actualJson,
        const std::vector<std::string>& errors);

    // JSON-escape a string and wrap it in double quotes.
    static std::string Quote(const std::string& value);
    static std::string Bool(bool value);
    static std::string Array(const std::vector<std::string>& values);

private:
    std::string m_root;
    std::string MatrixDir() const;
};

} // namespace notch_mock

#endif
