#include "verify/verify_byte_exact.h"

#include <sstream>

#include "case_logger.h"
#include "hash_utils.h"

namespace notch_mock
{
VerifyResult VerifyByteExact(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& delivered)
{
    const std::string expectedHash = Sha256Bytes(expected);
    const std::string deliveredHash = Sha256Bytes(delivered);
    VerifyResult result;
    result.ok = !delivered.empty() && expectedHash == deliveredHash;
    std::ostringstream detail;
    detail << "{\"comparison\":\"byte_exact\",\"expected_sha256\":" << CaseLogger::Quote(expectedHash)
           << ",\"delivered_sha256\":" << CaseLogger::Quote(deliveredHash)
           << ",\"expected_bytes\":" << expected.size()
           << ",\"delivered_bytes\":" << delivered.size() << "}";
    result.detail = detail.str();
    if (!result.ok)
    {
        result.error = "output_hash_mismatch";
    }
    return result;
}
}
