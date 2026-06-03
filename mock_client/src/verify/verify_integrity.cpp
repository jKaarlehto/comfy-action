#include "verify/verify_integrity.h"

#include <sstream>

#include "case_logger.h"
#include "hash_utils.h"

namespace notch_mock
{
VerifyResult VerifyIntegrity(const std::vector<uint8_t>& delivered, const std::string& serverSha256)
{
    VerifyResult result;
    const std::string deliveredHash = delivered.empty() ? "" : Sha256Bytes(delivered);
    const bool nonEmpty = !delivered.empty();
    const bool hashOk = serverSha256.empty() || deliveredHash == serverSha256;
    result.ok = nonEmpty && hashOk;

    std::ostringstream detail;
    detail << "{\"comparison\":\"integrity\",\"delivered_bytes\":" << delivered.size()
           << ",\"delivered_sha256\":" << CaseLogger::Quote(deliveredHash)
           << ",\"server_sha256\":" << CaseLogger::Quote(serverSha256)
           << ",\"server_hash_checked\":" << CaseLogger::Bool(!serverSha256.empty()) << "}";
    result.detail = detail.str();

    if (!nonEmpty)
    {
        result.error = "output_artifact_empty";
    }
    else if (!hashOk)
    {
        result.error = "output_hash_mismatch";
    }
    return result;
}
}
