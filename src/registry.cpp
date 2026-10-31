#include "registry.h"

#include <objbase.h>
#include <shlwapi.h>
#include <wil/registry.h>

namespace dfa::registry {
namespace {

LSTATUS Win32Status(HRESULT result) {
    if (SUCCEEDED(result)) {
        return ERROR_SUCCESS;
    }
    return HRESULT_FACILITY(result) == FACILITY_WIN32
        ? static_cast<LSTATUS>(HRESULT_CODE(result))
        : ERROR_GEN_FAILURE;
}

Result<void> Checked(LSTATUS status, std::wstring_view operation) {
    if (status != ERROR_SUCCESS) {
        return std::unexpected(Error{status, std::wstring(operation)});
    }
    return {};
}

Result<wil::unique_hkey> Open(HKEY root, std::wstring_view key, REGSAM access,
                               bool create = false) {
    // root 与 key 分开传递；如 root=HKCU、key=Software\Classes\SamplePdfReader.pdf，
    // 实际位置是 HKCU\Software\Classes\SamplePdfReader.pdf。key 本身不能再包含“HKCU\”。
    // WIL 的打开/创建帮助函数只提供固定权限选项，读写选项为 KEY_ALL_ACCESS。
    // 此处用原生 API 保留精确的 REGSAM 权限，键句柄仍由 WIL 自动释放。
    // Win7+ 的 HKCU\Software 和普通 Classes 文件关联键在 WOW64 下共享。
    // 不手工拼 Wow6432Node，也不统一加 KEY_WOW64_64KEY；COM 的 CLSID/Interface 不在本工具操作范围内。
    wil::unique_hkey handle;
    const std::wstring path(key);
    const LSTATUS status = create
        ? ::RegCreateKeyExW(root, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                             access, nullptr, handle.put(), nullptr)
        : ::RegOpenKeyExW(root, path.c_str(), 0, access, handle.put());
    if (status != ERROR_SUCCESS) {
        return std::unexpected(Error{status, create ? L"RegCreateKeyExW" : L"RegOpenKeyExW"});
    }
    return handle;
}

template <typename Iterator>
Result<std::vector<std::wstring>> Names(HKEY root, std::wstring_view key) {
    // 枚举的是值名称或直接子键名称，既不读取值数据，也不递归遍历子树。
    // 例如 .pdf\OpenWithProgids 中得到 SamplePdfReader.pdf；OpenWithList 中得到 samplepdfreader.exe。
    auto handle = Open(root, key, KEY_READ);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    std::vector<std::wstring> names;
    Iterator iterator(handle->get());
    for (; iterator != Iterator{}; ++iterator) {
        names.emplace_back(iterator->name.get());
    }
    if (FAILED(iterator.last_error())) {
        return std::unexpected(Error{Win32Status(iterator.last_error()), L"Enumerate registry names"});
    }
    return names;
}

} // namespace

Result<std::vector<std::wstring>> ValueNames(HKEY root, std::wstring_view key) {
    return Names<wil::reg::value_heap_string_nothrow_iterator>(root, key);
}

Result<std::vector<std::wstring>> SubkeyNames(HKEY root, std::wstring_view key) {
    return Names<wil::reg::key_heap_string_nothrow_iterator>(root, key);
}

Result<void> SetDword(HKEY root, std::wstring_view key,
                      std::wstring_view value_name, DWORD value) {
    // value_name 是键中的字段名，不是额外子键；用于写确认记录的 REG_DWORD 数据。
    auto handle = Open(root, key, KEY_SET_VALUE, true);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    return Checked(Win32Status(wil::reg::set_value_dword_nothrow(
                       handle->get(), std::wstring(value_name).c_str(), value)),
                   L"Set registry DWORD");
}

Result<void> SetString(HKEY root, std::wstring_view key,
                       std::wstring_view value_name, std::wstring_view value) {
    // 空 value_name 表示“默认值”。WIL 负责写入带终止符的 Unicode REG_SZ。
    // 例如 key=Software\Classes\SamplePdfReader.pdf\shell\open\command、value_name=""，
    // value 是 "C:\Apps\SamplePdfReader\SamplePdfReader.exe" "%1" %*，而不是要继续拼接的子键。
    auto handle = Open(root, key, KEY_SET_VALUE, true);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    return Checked(Win32Status(wil::reg::set_value_string_nothrow(
                       handle->get(), std::wstring(value_name).c_str(), std::wstring(value).c_str())),
                   L"Set registry string");
}

Result<void> SetEmptyValue(HKEY root, std::wstring_view key, std::wstring_view value_name) {
    // OpenWithProgids 要求保留 REG_NONE + 零字节，不能换成 REG_SZ 空字符串。
    auto handle = Open(root, key, KEY_SET_VALUE, true);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    return Checked(::RegSetValueExW(handle->get(), std::wstring(value_name).c_str(),
                                     0, REG_NONE, nullptr, 0),
                   L"Set OpenWithProgids entry");
}

Result<void> DeleteValue(HKEY root, std::wstring_view key, std::wstring_view value_name) {
    // 只删除指定值，不删除它所在的键；不存在视为已经满足清理目标。
    const LSTATUS status = ::RegDeleteKeyValueW(root, std::wstring(key).c_str(),
                                                std::wstring(value_name).c_str());
    return Checked(status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND
                       ? ERROR_SUCCESS : status,
                   L"RegDeleteKeyValueW");
}

Result<void> DeleteTree(HKEY root, std::wstring_view key) {
    // 只对调用方传入的键清理；常见目标为 HKCU\...\FileExts\.pdf\UserChoice。
    const std::wstring path(key);
    LSTATUS status = ::SHDeleteKeyW(root, path.c_str());
    std::wstring_view operation = L"SHDeleteKeyW";
    if (status == ERROR_ACCESS_DENIED) {
        // 递归删除会要求额外权限；叶子键只需 DELETE，不必清空整个 DACL。
        status = ::RegDeleteKeyW(root, path.c_str());
        operation = L"RegDeleteKeyW";
    }
    return Checked(status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND
                       ? ERROR_SUCCESS : status,
                   operation);
}

Result<std::wstring> ReadString(HKEY root, std::wstring_view key, std::wstring_view value_name) {
    // 缺失、类型错误均返回 Error；真实存在的空字符串则返回成功的空 wstring。
    wil::unique_cotaskmem_string value;
    const HRESULT result = wil::reg::get_value_string_nothrow(
        root, std::wstring(key).c_str(), std::wstring(value_name).c_str(), value);
    if (FAILED(result)) {
        return std::unexpected(Error{Win32Status(result), L"Read registry string"});
    }
    return std::wstring(value.get());
}

Result<FILETIME> LastWriteTime(HKEY root, std::wstring_view key) {
    // 读取的是键的最后写入时间，而不是某个值的时间；UserChoice 哈希校验依赖此信息。
    auto handle = Open(root, key, KEY_QUERY_VALUE);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    FILETIME timestamp{};
    auto checked = Checked(::RegQueryInfoKeyW(handle->get(), nullptr, nullptr, nullptr,
                                              nullptr, nullptr, nullptr, nullptr, nullptr,
                                              nullptr, nullptr, &timestamp),
                            L"RegQueryInfoKeyW");
    if (!checked) {
        return std::unexpected(checked.error());
    }
    return timestamp;
}

std::wstring DescribeError(const Error& error) {
    return error.operation + L" failed with Win32 error " + std::to_wstring(error.status);
}

} // namespace dfa::registry
