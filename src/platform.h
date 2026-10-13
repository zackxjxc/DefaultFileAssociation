#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <filesystem>

namespace dfa::platform {

struct WindowsVersion {
    DWORD major = 0;
    DWORD minor = 0;
    DWORD build = 0;

    [[nodiscard]] bool IsWindows8OrGreater() const noexcept;
    [[nodiscard]] bool IsWindows10OrGreater() const noexcept;
};

[[nodiscard]] WindowsVersion GetWindowsVersion() noexcept;
// 判断操作系统位数，不是当前进程位数；x86 程序在 WOW64 中也可能返回 true。
[[nodiscard]] bool Is64BitWindows() noexcept;
// 原生系统的 mshta 路径；x86/WOW64 使用 Sysnative 别名访问 64 位宿主。
[[nodiscard]] std::filesystem::path GetMshtaPath();
[[nodiscard]] bool IsProcessElevated() noexcept;
[[nodiscard]] std::wstring GetCurrentUserSid();
[[nodiscard]] std::wstring FormatWindowsError(DWORD error);

[[nodiscard]] std::wstring ToLower(std::wstring value);
[[nodiscard]] bool EqualsInsensitive(std::wstring_view left, std::wstring_view right) noexcept;
[[nodiscard]] std::wstring QuoteCommandLineArgument(std::wstring_view value);

} // namespace dfa::platform
