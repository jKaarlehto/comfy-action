#include "hash_utils.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace notch_mock
{

namespace
{

uint32_t Rotr(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

uint32_t Ch(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~x & z);
}

uint32_t Maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

uint32_t BigSigma0(uint32_t x)
{
    return Rotr(x, 2U) ^ Rotr(x, 13U) ^ Rotr(x, 22U);
}

uint32_t BigSigma1(uint32_t x)
{
    return Rotr(x, 6U) ^ Rotr(x, 11U) ^ Rotr(x, 25U);
}

uint32_t SmallSigma0(uint32_t x)
{
    return Rotr(x, 7U) ^ Rotr(x, 18U) ^ (x >> 3U);
}

uint32_t SmallSigma1(uint32_t x)
{
    return Rotr(x, 17U) ^ Rotr(x, 19U) ^ (x >> 10U);
}

const uint32_t kRoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

std::string Sha256Raw(const uint8_t* data, size_t size)
{
    uint32_t h[8] = {
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U,
    };

    std::vector<uint8_t> message;
    if (size > 0U && data != nullptr)
    {
        message.assign(data, data + size);
    }
    const uint64_t bitLength = static_cast<uint64_t>(size) * 8ULL;
    message.push_back(0x80U);
    while ((message.size() % 64U) != 56U)
    {
        message.push_back(0U);
    }
    for (int i = 7; i >= 0; --i)
    {
        message.push_back(static_cast<uint8_t>((bitLength >> (static_cast<uint64_t>(i) * 8ULL)) & 0xffU));
    }

    for (size_t offset = 0; offset < message.size(); offset += 64U)
    {
        uint32_t w[64];
        for (size_t i = 0; i < 16U; ++i)
        {
            const size_t j = offset + i * 4U;
            w[i] = (static_cast<uint32_t>(message[j]) << 24U) |
                   (static_cast<uint32_t>(message[j + 1U]) << 16U) |
                   (static_cast<uint32_t>(message[j + 2U]) << 8U) |
                   static_cast<uint32_t>(message[j + 3U]);
        }
        for (size_t i = 16U; i < 64U; ++i)
        {
            w[i] = SmallSigma1(w[i - 2U]) + w[i - 7U] + SmallSigma0(w[i - 15U]) + w[i - 16U];
        }

        uint32_t a = h[0];
        uint32_t b = h[1];
        uint32_t c = h[2];
        uint32_t d = h[3];
        uint32_t e = h[4];
        uint32_t f = h[5];
        uint32_t g = h[6];
        uint32_t hh = h[7];

        for (size_t i = 0; i < 64U; ++i)
        {
            const uint32_t t1 = hh + BigSigma1(e) + Ch(e, f, g) + kRoundConstants[i] + w[i];
            const uint32_t t2 = BigSigma0(a) + Maj(a, b, c);
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (size_t i = 0; i < 8U; ++i)
    {
        stream << std::setw(8) << h[i];
    }
    return stream.str();
}

} // namespace

bool ReadBinaryFile(const std::string& path, std::vector<uint8_t>& bytes, std::string& error)
{
    bytes.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "could not open file: " + path;
        return false;
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff end = stream.tellg();
    if (end < 0)
    {
        error = "could not size file: " + path;
        return false;
    }
    stream.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(end));
    if (!bytes.empty())
    {
        stream.read(reinterpret_cast<char*>(&bytes[0]), static_cast<std::streamsize>(bytes.size()));
        if (!stream)
        {
            error = "could not read file: " + path;
            return false;
        }
    }
    return true;
}

bool WriteTextFile(const std::string& path, const std::string& text, std::string& error)
{
    std::ofstream stream(path);
    if (!stream)
    {
        error = "could not write file: " + path;
        return false;
    }
    stream << text;
    return true;
}

std::string Sha256Bytes(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty())
    {
        return Sha256Raw(nullptr, 0U);
    }
    return Sha256Raw(&bytes[0], bytes.size());
}

std::string Sha256String(const std::string& text)
{
    if (text.empty())
    {
        return Sha256Raw(nullptr, 0U);
    }
    return Sha256Raw(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

} // namespace notch_mock
