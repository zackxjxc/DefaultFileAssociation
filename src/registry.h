#pragma once

#include <windows.h>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace dfa::registry {

struct Error {
    LSTATUS status = ERROR_SUCCESS;
    std::wstring operation;
};

template <typename T>
using Result = std::expected<T, Error>;

// 所有 key 都是相对 root 的注册表路径；如 HKCU + Software\Classes\.pdf。
// valueName 是该键内的值名称，空字符串表示默认值；调用方决定操作哪个根键。
[[nodiscard]] Result<void> SetString(HKEY root, std::wstring_view key,
                                     std::wstring_view valueName, std::wstring_view value);
[[nodiscard]] Result<void> SetEmptyValue(HKEY root, std::wstring_view key,
                                         std::wstring_view valueName);
[[nodiscard]] Result<void> SetDword(HKEY root, std::wstring_view key,
                                    std::wstring_view value_name, DWORD value);
[[nodiscard]] Result<std::vector<std::wstring>> ValueNames(HKEY root, std::wstring_view key);
[[nodiscard]] Result<std::vector<std::wstring>> SubkeyNames(HKEY root, std::wstring_view key);
[[nodiscard]] Result<void> DeleteValue(HKEY root, std::wstring_view key,
                                       std::wstring_view valueName);
[[nodiscard]] Result<void> DeleteTree(HKEY root, std::wstring_view key);
[[nodiscard]] Result<std::wstring> ReadString(HKEY root, std::wstring_view key,
                                              std::wstring_view valueName);
// 返回键的真实 FILETIME，用于按最后写入分钟重新验证 UserChoice 哈希。
[[nodiscard]] Result<FILETIME> LastWriteTime(HKEY root, std::wstring_view key);

[[nodiscard]] std::wstring DescribeError(const Error& error);

} // namespace dfa::registry
