#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "hash_utils.h"
#include "verify/verify_byte_exact.h"
#include "verify/verify_integrity.h"
#include "verify/verify_structural.h"

namespace
{
std::vector<uint8_t> Bytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

void Put32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(uint8_t(value & 0xFF));
    out.push_back(uint8_t((value >> 8) & 0xFF));
    out.push_back(uint8_t((value >> 16) & 0xFF));
    out.push_back(uint8_t((value >> 24) & 0xFF));
}

// Minimal valid GLB v2: header + one JSON chunk.
std::vector<uint8_t> MinimalGlb(const std::string& json)
{
    std::vector<uint8_t> chunk = Bytes(json);
    std::vector<uint8_t> glb;
    const uint32_t total = 12 + 8 + uint32_t(chunk.size());
    Put32(glb, 0x46546C67u);  // 'glTF'
    Put32(glb, 2u);           // version
    Put32(glb, total);        // total length
    Put32(glb, uint32_t(chunk.size()));
    Put32(glb, 0x4E4F534Au);  // 'JSON'
    glb.insert(glb.end(), chunk.begin(), chunk.end());
    return glb;
}
}  // namespace

int main()
{
    // --- byte-exact ---
    std::vector<uint8_t> a = {1, 2, 3, 4};
    std::vector<uint8_t> b = {1, 2, 3, 4};
    std::vector<uint8_t> c = {1, 2, 3, 5};
    assert(notch_mock::VerifyByteExact(a, b).ok);
    notch_mock::VerifyResult diff = notch_mock::VerifyByteExact(a, c);
    assert(!diff.ok);
    assert(diff.error == "output_hash_mismatch");
    assert(!notch_mock::VerifyByteExact(a, std::vector<uint8_t>{}).ok);

    // --- integrity: non-empty + optional server hash ---
    std::vector<uint8_t> payload = {9, 8, 7, 6};
    assert(notch_mock::VerifyIntegrity(payload).ok);
    notch_mock::VerifyResult emptyArtifact = notch_mock::VerifyIntegrity(std::vector<uint8_t>{});
    assert(!emptyArtifact.ok);
    assert(emptyArtifact.error == "output_artifact_empty");
    assert(notch_mock::VerifyIntegrity(payload, notch_mock::Sha256Bytes(payload)).ok);
    notch_mock::VerifyResult hashMismatch = notch_mock::VerifyIntegrity(payload, notch_mock::Sha256Bytes(a));
    assert(!hashMismatch.ok);
    assert(hashMismatch.error == "output_hash_mismatch");

    // --- structural: GLB + JSON validity, reserialization-tolerant ---
    assert(notch_mock::VerifyStructural(MinimalGlb("{\"asset\":{\"version\":\"2.0\"}}")).ok);
    std::vector<uint8_t> notGlb = Bytes("glTFnope");  // magic only, truncated header
    assert(!notch_mock::VerifyStructural(notGlb).ok);
    assert(notch_mock::VerifyStructural(Bytes("{\"position\":[0,1,2],\"target\":[0,0,0]}")).ok);
    assert(notch_mock::VerifyStructural(Bytes("[1, 2, [3, 4]]")).ok);
    assert(!notch_mock::VerifyStructural(Bytes("{\"a\":")).ok);          // unterminated
    assert(!notch_mock::VerifyStructural(Bytes("not json at all")).ok);  // not object/array
    assert(!notch_mock::VerifyStructural(std::vector<uint8_t>{}).ok);    // empty

    return 0;
}
