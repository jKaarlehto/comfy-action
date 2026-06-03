#ifndef NOTCH_MOCK_VERIFY_INTEGRITY_H
#define NOTCH_MOCK_VERIFY_INTEGRITY_H

#include <cstdint>
#include <string>
#include <vector>

#include "matrix.h"  // VerifyResult

namespace notch_mock
{
// Delivered-artifact integrity for transcoded types (audio, video): the output
// must be non-empty and, when the server reported a hash, match it. The positive
// case already proves the WS ready event + a successful fetch upstream, so this
// confirms a real, non-empty artifact came back.
VerifyResult VerifyIntegrity(const std::vector<uint8_t>& delivered, const std::string& serverSha256 = "");
}

#endif
