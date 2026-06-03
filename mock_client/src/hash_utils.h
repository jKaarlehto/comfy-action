#ifndef NOTCH_MOCK_HASH_UTILS_H
#define NOTCH_MOCK_HASH_UTILS_H

#include <stdint.h>

#include <string>
#include <vector>

namespace notch_mock
{

bool ReadBinaryFile(const std::string& path, std::vector<uint8_t>& bytes, std::string& error);
bool WriteTextFile(const std::string& path, const std::string& text, std::string& error);
std::string Sha256Bytes(const std::vector<uint8_t>& bytes);
std::string Sha256String(const std::string& text);

} // namespace notch_mock

#endif
