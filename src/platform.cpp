#include "platform.h"

#include <sddl.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>
#include <vector>

namespace dfa::platform {
namespace {

using RtlGetNtVersionNumbers = VOID(WINAPI*)(DWORD*, DWORD*, DWORD*);

} // namespace

bool WindowsVersion::IsWindows8OrGreater() const noexcept {
    return major > 6 || (major == 6 && minor >= 2);
}

bool WindowsVersion::IsWindows10OrGreater() const noexcept {
    return major >= 10;
}

WindowsVersion GetWindowsVersion() noexcept {
    // 避免版本兼容清单影响判断；入口动态解析，不增加较新系统 API 的静态依赖。
    WindowsVersion result;
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto getVersion = reinterpret_cast<RtlGetNtVersionNumbers>(
            ::GetProcAddress(ntdll, "RtlGetNtVersionNumbers"));
        if (getVersion) {
            getVersion(&result.major, &result.minor, &result.build);
            result.build &= 0xFFFFu;
            return result;
        }
    }

    OSVERSIONINFOW version{sizeof(version)};
#pragma warning(push)
#pragma warning(disable : 4996)
    if (::GetVersionExW(&version)) {
        result.major = version.dwMajorVersion;
        result.minor = version.dwMinorVersion;
        result.build = version.dwBuildNumber;
    }
#pragma warning(pop)
    return result;
}

bool Is64BitWindows() noexcept {
    SYSTEM_INFO system{};
    ::GetNativeSystemInfo(&system);
    return system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
           system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ||
           system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64;
}

std::filesystem::path GetMshtaPath() {
    wil::unique_cotaskmem_string directory;
    if (FAILED(wil::GetSystemDirectoryW(directory))) {
        return {};
    }
    std::filesystem::path path(directory.get());
#ifndef _WIN64
    if (Is64BitWindows()) {
        // C:\Windows\System32 -> C:\Windows\Sysnative：仅 WOW64 进程可用的原生目录别名。
        // 不全局禁用重定向，避免影响运行库加载；此路径只用于启动，不写入阅读器关联。
        path = path.parent_path() / L"Sysnative";
    }
#endif
    return path / L"mshta.exe";
}

bool IsProcessElevated() noexcept {
    // 只检查当前进程是否提升，不判断账户是否属于 Administrators 组。
    // 管理员账户也可以运行未提升进程；本工具不降权、不提权、不切换用户。
    wil::unique_handle token;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.addressof())) {
        // 无法确认权限时停止执行，不能误写入管理员账户的配置。
        return true;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (!::GetTokenInformation(token.get(), TokenElevation, &elevation, size, &size)) {
        return true;
    }
    return elevation.TokenIsElevated != FALSE;
}

std::wstring GetCurrentUserSid() {
    // 查询进程 token 的 SID，确保哈希和当前进程访问的 HKCU 使用同一用户身份。
    // 不使用线程模拟身份，也不依据用户名猜测用户 SID。
    wil::unique_handle token;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.addressof())) {
        return {};
    }

    DWORD size = 0;
    if (::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size) ||
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }
    // WIL 的令牌查询帮助函数要求 Win8；保留进程 token 查询，用容器管理缓冲区。
    std::vector<BYTE> buffer(size);
    auto* token_user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    if (!::GetTokenInformation(token.get(), TokenUser, token_user, size, &size)) {
        return {};
    }

    wil::unique_hlocal_string sid;
    if (!::ConvertSidToStringSidW(token_user->User.Sid, sid.addressof())) {
        return {};
    }
    return sid.get();
}

std::wstring FormatWindowsError(DWORD error) {
    // 系统返回的 Unicode 错误说明用于展示；去掉末尾换行以便调用方统一排版。
    if (error == ERROR_SUCCESS) {
        return {};
    }
    LPWSTR raw = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = ::FormatMessageW(flags, nullptr, error, 0,
                                    reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    wil::unique_hlocal_string message(raw);
    if (!message || length == 0) {
        return L"Windows error " + std::to_wstring(error);
    }
    std::wstring result(message.get(), length);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

std::wstring ToLower(std::wstring value) {
    // 与 Windows 的 UserChoice 输入规则一致，不能依赖 CRT 的默认 C 区域设置。
    if (!value.empty()) {
        ::CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    }
    return value;
}

bool EqualsInsensitive(std::wstring_view left, std::wstring_view right) noexcept {
    // 使用 Windows 的序数大小写比较；不依赖 CRT 当前语言区域设置。
    return left.size() == right.size() &&
           ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                  right.data(), static_cast<int>(right.size()), TRUE) ==
               CSTR_EQUAL;
}

std::wstring QuoteCommandLineArgument(std::wstring_view value) {
    // 为 Windows 参数生成一个完整带引号的字符串，例如 C:\Apps\Doc Box\DocBox.exe
    // 变成 "C:\Apps\Doc Box\DocBox.exe"；这里只编码参数，不解析或访问文件。
    // 引号前和参数结尾的反斜杠需要按 Windows 参数规则加倍，不能简单在两端加引号。
    std::wstring result;
    result.reserve(value.size() + 2);
    result.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(c);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

} // namespace dfa::platform
