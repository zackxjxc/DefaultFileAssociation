#pragma once

#include "platform.h"

#include <string>

namespace dfa::crypto {

// Windows 10 changed the UserChoice input by appending a Windows User
// Experience marker and a build-specific GUID. Windows 11 continues to use
// the post-Threshold-2 variant.
enum class HashVersion {
    Legacy,
    Windows10Threshold1,
    Windows10Threshold2OrLater,
};

struct HashResult {
    bool ok = false;
    std::wstring value;
    std::wstring input;
    std::wstring error;
};

[[nodiscard]] HashVersion SelectHashVersion(const platform::WindowsVersion& version) noexcept;

// Generates the UserChoice value used by the legacy registration path. The
// extension, ProgID, and SID are concatenated as UTF-16, a terminating NUL is
// included in the hash input, and the final value is standard Base64.
//
// This algorithm is an interoperability implementation of the published
// Windows UserChoice research. It is not a general-purpose password hash.
[[nodiscard]] HashResult GenerateUserChoiceHash(
    const std::wstring& extension,
    const std::wstring& prog_id,
    const std::wstring& user_sid,
    const SYSTEMTIME& timestamp,
    const platform::WindowsVersion& version);

[[nodiscard]] HashResult GenerateUserChoiceHash(
    const std::wstring& extension,
    const std::wstring& prog_id,
    const std::wstring& user_sid,
    const SYSTEMTIME& timestamp,
    HashVersion version);

} // namespace dfa::crypto
