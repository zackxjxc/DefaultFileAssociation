#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace dfa::crypto {

// Windows CNG MD5, used only as one stage of the UserChoice hash algorithm.
// Windows API failures throw wil::ResultException.
[[nodiscard]] std::array<std::byte, 16> Md5(std::span<const std::byte> data);

// Encodes binary data with the RFC 4648 standard Base64 alphabet. The result
// has no line breaks and is implemented with CryptBinaryToStringW from
// crypt32.dll. Windows API failures throw wil::ResultException.
[[nodiscard]] std::wstring Base64Encode(std::span<const std::byte> data);

} // namespace dfa::crypto
