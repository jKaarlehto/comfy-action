#include "verify/verify_structural.h"

#include <cstdint>
#include <sstream>
#include <string>

#include "case_logger.h"

namespace notch_mock
{
namespace
{
uint32_t LeU32(const std::vector<uint8_t>& bytes, size_t offset)
{
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) | (uint32_t(bytes[offset + 2]) << 16) |
           (uint32_t(bytes[offset + 3]) << 24);
}

// GLB v2: 12-byte header [magic 'glTF'=0x46546C67, version=2, total length] then
// chunks [length, type, data]; the first chunk must be a non-empty JSON chunk
// (type 0x4E4F534A). Pure byte inspection, no JSON parser.
bool ValidateGlb(const std::vector<uint8_t>& bytes, std::string& note)
{
    if (bytes.size() < 20)
    {
        note = "glb smaller than a v2 header + json chunk header";
        return false;
    }
    if (LeU32(bytes, 0) != 0x46546C67u)
    {
        note = "missing glTF magic";
        return false;
    }
    if (LeU32(bytes, 4) != 2u)
    {
        note = "glb version is not 2";
        return false;
    }
    if (LeU32(bytes, 8) != bytes.size())
    {
        note = "glb declared length does not match byte count";
        return false;
    }
    const uint32_t jsonLength = LeU32(bytes, 12);
    if (LeU32(bytes, 16) != 0x4E4F534Au)
    {
        note = "first chunk is not a JSON chunk";
        return false;
    }
    if (jsonLength == 0 || size_t(20) + jsonLength > bytes.size())
    {
        note = "empty or oversized JSON chunk";
        return false;
    }
    note = "valid glb v2, json chunk " + std::to_string(jsonLength) + " bytes";
    return true;
}

// Light JSON well-formedness: non-empty, starts with { or [, and braces/brackets
// balance outside of strings. Not a full parser — enough to prove a coherent doc
// (the camera payload is a reserialized dict, not byte-stable).
bool ValidateJson(const std::vector<uint8_t>& bytes, std::string& note)
{
    size_t i = 0;
    while (i < bytes.size() && (bytes[i] == ' ' || bytes[i] == '\n' || bytes[i] == '\t' || bytes[i] == '\r'))
    {
        ++i;
    }
    if (i >= bytes.size() || (bytes[i] != '{' && bytes[i] != '['))
    {
        note = "not a json object/array";
        return false;
    }
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (; i < bytes.size(); ++i)
    {
        const char c = char(bytes[i]);
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == '"')
            {
                inString = false;
            }
            continue;
        }
        if (c == '"')
        {
            inString = true;
        }
        else if (c == '{' || c == '[')
        {
            ++depth;
        }
        else if (c == '}' || c == ']')
        {
            --depth;
            if (depth < 0)
            {
                note = "unbalanced json (early close)";
                return false;
            }
        }
    }
    if (depth != 0 || inString)
    {
        note = "unterminated json";
        return false;
    }
    note = "balanced json " + std::to_string(bytes.size()) + " bytes";
    return true;
}
}  // namespace

VerifyResult VerifyStructural(const std::vector<uint8_t>& delivered)
{
    VerifyResult result;
    std::string note;
    std::string kind;
    bool ok = false;
    if (delivered.size() >= 4 && delivered[0] == 'g' && delivered[1] == 'l' && delivered[2] == 'T' &&
        delivered[3] == 'F')
    {
        kind = "glb";
        ok = ValidateGlb(delivered, note);
    }
    else
    {
        kind = "json";
        ok = ValidateJson(delivered, note);
    }
    result.ok = ok && !delivered.empty();

    std::ostringstream detail;
    detail << "{\"comparison\":\"structural\",\"kind\":" << CaseLogger::Quote(kind)
           << ",\"delivered_bytes\":" << delivered.size() << ",\"note\":" << CaseLogger::Quote(note) << "}";
    result.detail = detail.str();
    if (!result.ok)
    {
        result.error = "structural_invalid";
    }
    return result;
}
}
