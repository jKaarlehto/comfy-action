#ifndef NOTCH_MOCK_VERIFY_BYTE_EXACT_H
#define NOTCH_MOCK_VERIFY_BYTE_EXACT_H

#include <cstdint>
#include <vector>

#include "matrix.h"  // VerifyResult

namespace notch_mock
{
// SHA-256 equality of two byte buffers. detail carries both hashes + sizes.
// ok is false when the delivered buffer is empty or the hashes differ.
VerifyResult VerifyByteExact(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& delivered);
}

#endif
