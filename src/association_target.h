#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace dfa::association {

// SDK 在 Win7 目标下不声明 ASSOCF_IS_PROTOCOL（0x1000）；仅在运行于 Win8+ 时传此标志。
inline constexpr unsigned kProtocolQueryFlag = 0x1000;

// 带点的是文件后缀（.html），不带点的是协议（https）；只接受经过 CLI 校验的标识。
inline bool IsProtocol(std::wstring_view identifier) {
    return !identifier.empty() && identifier.front() != L'.';
}

inline std::wstring ChoiceParent(std::wstring_view identifier) {
    // 文件：HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf。
    // 协议：HKCU\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\https。
    return std::wstring(IsProtocol(identifier)
        ? L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\"
        : L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\") + std::wstring(identifier);
}

inline std::wstring ChoiceKey(std::wstring_view identifier) {
    return ChoiceParent(identifier) + L"\\UserChoice";
}

inline bool IsValidProgId(std::wstring_view value) {
    return !value.empty() && value.size() <= 255 &&
        std::ranges::all_of(value, [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                   (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_';
        });
}

} // namespace dfa::association
