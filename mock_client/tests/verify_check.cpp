#include <cassert>
#include <vector>

#include "verify/verify_byte_exact.h"

int main()
{
    std::vector<uint8_t> a = {1, 2, 3, 4};
    std::vector<uint8_t> b = {1, 2, 3, 4};
    std::vector<uint8_t> c = {1, 2, 3, 5};

    notch_mock::VerifyResult same = notch_mock::VerifyByteExact(a, b);
    assert(same.ok);

    notch_mock::VerifyResult diff = notch_mock::VerifyByteExact(a, c);
    assert(!diff.ok);
    assert(diff.error == "output_hash_mismatch");

    notch_mock::VerifyResult empty = notch_mock::VerifyByteExact(a, std::vector<uint8_t>{});
    assert(!empty.ok);

    return 0;
}
