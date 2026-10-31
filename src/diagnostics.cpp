#include "diagnostics.h"

#include "crypto/user_choice_hash.h"
#include "platform.h"
#include "registry.h"
#include "association_target.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wil/com.h>

#include <cstring>
#include <cwchar>
#include <format>
#include <vector>

namespace dfa::diagnostics {
namespace {


void DumpKey(std::wstring& output, HKEY root, const std::wstring& path,
             std::wstring_view label, std::wstring_view suffix = L"") {
    // label 仅用于日志展示，root 才决定实际访问的根键；path 是根键下的相对路径。
    // suffix 过滤值名称：例如“.pdf”对应的确认记录后缀是“_.pdf”。
    output += std::format(L"[{}\\{}]\n", label, path);
    wil::unique_hkey key;
    // 关联键共享；只有 HKLM 系统版本诊断显式选择原生视图，避免 x86 读到不完整版本字段。
    const bool system_version = root == HKEY_LOCAL_MACHINE &&
        path == L"Software\\Microsoft\\Windows NT\\CurrentVersion";
    const LSTATUS opened = ::RegOpenKeyExW(root, path.c_str(), 0,
        KEY_READ | (system_version ? KEY_WOW64_64KEY : 0), key.put());
    if (opened != ERROR_SUCCESS) {
        output += std::format(L"  open_error={}\n", opened);
        return;
    }
    // 对已打开句柄查询，保证时间、名称和数据使用同一个注册表视图。
    auto timestamp = registry::LastWriteTime(key.get(), L"");
    if (timestamp) {
        output += std::format(L"  last_write_filetime={:08x}{:08x}\n",
                               timestamp->dwHighDateTime, timestamp->dwLowDateTime);
    }
    if (path.ends_with(L"\\UserChoice")) {
        // 记录所有者与 DACL 的 SDDL 文本，帮助区分普通写入拒绝和其他系统保护。
        // 此处仅读取权限，不修改权限，也不改变键的最后写入时间。
        wil::unique_hlocal_security_descriptor descriptor;
        const DWORD status = ::GetSecurityInfo(key.get(), SE_REGISTRY_KEY,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, nullptr, descriptor.put());
        wil::unique_hlocal_string sddl;
        if (status == ERROR_SUCCESS && ::ConvertSecurityDescriptorToStringSecurityDescriptorW(
                descriptor.get(), SDDL_REVISION_1,
                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, sddl.put(), nullptr)) {
            output += L"  security=" + std::wstring(sddl.get()) + L"\n";
        } else {
            output += std::format(L"  security_error={}\n", status == ERROR_SUCCESS ? ::GetLastError() : status);
        }
    }
    auto names = registry::ValueNames(key.get(), L"");
    if (!names) {
        output += L"  " + registry::DescribeError(names.error()) + L"\n";
        return;
    }
    for (const auto& name : *names) {
        // 版本诊断只收集版本字段，不把产品密钥、注册人等无关信息写进日志。
        if (root == HKEY_LOCAL_MACHINE && path == L"Software\\Microsoft\\Windows NT\\CurrentVersion" &&
            name != L"ProductName" && name != L"ReleaseId" && name != L"DisplayVersion" &&
            name != L"CurrentBuildNumber" && name != L"UBR" && name != L"InstallationType") {
            continue;
        }
        if (!suffix.empty() && !name.ends_with(suffix)) {
            continue;
        }
        DWORD type = 0;
        DWORD size = 0;
        // 先查询所需字节数，再读取；诊断每个值最多接受 16 KiB，避免无关大数据撑爆日志。
        // 两次调用之间值可能变化，读取失败时报告错误，不解释未成功读取的缓冲区。
        LSTATUS status = ::RegQueryValueExW(key.get(), name.c_str(), nullptr, &type, nullptr, &size);
        if (status != ERROR_SUCCESS || size > 16 * 1024) {
            output += std::format(L"  {}: query_error={}, bytes={}\n", name, status, size);
            continue;
        }
        std::vector<BYTE> data(size + sizeof(wchar_t), 0);
        status = ::RegQueryValueExW(key.get(), name.c_str(), nullptr, &type, data.data(), &size);
        output += std::format(L"  {}: type={}, ", name.empty() ? L"(Default)" : name, type);
        if (status != ERROR_SUCCESS) {
            output += std::format(L"read_error={}\n", status);
        } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
            // 注册表可能含奇数字节或未终止的字符串，不能按 NUL 无界扫描。
            if (size % sizeof(wchar_t) != 0) {
                output += L"invalid_utf16_byte_count\n";
                continue;
            }
            std::wstring value(size / sizeof(wchar_t), L'\0');
            if (size != 0) {
                std::memcpy(value.data(), data.data(), size);
            }
            const auto end = value.find(L'\0');
            if (end != std::wstring::npos) {
                value.resize(end);
            }
            output += value + L"\n";
        } else if (type == REG_DWORD && size == sizeof(DWORD)) {
            DWORD value = 0;
            std::memcpy(&value, data.data(), sizeof(value));
            output += std::format(L"0x{:08x}\n", value);
        } else {
            output += std::format(L"bytes={}", size);
            for (DWORD index = 0; index < size && index < 128; ++index) {
                output += std::format(L" {:02x}", data[index]);
            }
            output += size > 128 ? L" (truncated)\n" : L"\n";
        }
    }
}

void DumpShell(std::wstring& output, const association::Specification& specification) {
    // ProgID 和命令分开查询并记录；即使前一项失败，后一项仍能提供诊断线索。
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wil::unique_couninitialize_call cleanup(SUCCEEDED(initialized));
    wil::com_ptr<IApplicationAssociationRegistration> registration;
    HRESULT status = FAILED(initialized) && initialized != RPC_E_CHANGED_MODE ? initialized : S_OK;
    if (SUCCEEDED(status)) {
        try {
            registration = wil::CoCreateInstance<IApplicationAssociationRegistration>(
                __uuidof(ApplicationAssociationRegistration));
        } catch (const wil::ResultException& error) {
            status = error.GetErrorCode();
        }
    }
    wil::unique_cotaskmem_string prog_id;
    if (SUCCEEDED(status)) {
        status = registration->QueryCurrentDefault(specification.extension.c_str(),
            association::IsProtocol(specification.extension) ? AT_URLPROTOCOL : AT_FILEEXTENSION,
            AL_EFFECTIVE, prog_id.put());
    }
    output += std::format(L"Shell.effective_progid: hr=0x{:08x}, value={}\n",
                          static_cast<DWORD>(status), prog_id ? prog_id.get() : L"(none)");
    // 不依赖 CheckDefault 的提前返回，始终记录 Shell 的独立查询结果。
    DWORD size = 0;
    const bool modern_protocol = association::IsProtocol(specification.extension) &&
        platform::GetWindowsVersion().IsWindows8OrGreater();
    const auto flags = modern_protocol ? static_cast<ASSOCF>(association::kProtocolQueryFlag) : ASSOCF_NONE;
    const auto* identifier = association::IsProtocol(specification.extension) && !modern_protocol && prog_id
        ? prog_id.get() : specification.extension.c_str();
    status = ::AssocQueryStringW(flags, ASSOCSTR_COMMAND,
                                  identifier, L"open", nullptr, &size);
    std::wstring command;
    if (SUCCEEDED(status) && size > 0) {
        command.resize(size);
        status = ::AssocQueryStringW(flags, ASSOCSTR_COMMAND,
                                      identifier, L"open", command.data(), &size);
        if (SUCCEEDED(status)) {
            command.resize(std::wcslen(command.c_str()));
        } else {
            command.clear();
        }
    }
    output += std::format(L"Shell.open_command: hr=0x{:08x}, value={}\n",
                          static_cast<DWORD>(status), command.empty() ? L"(none)" : command);
}

void DumpHashProfile(std::wstring& output, const association::Specification& specification,
                     const platform::WindowsVersion& version, std::wstring_view sid) {
    // 版本识别只读系统保护数据和 UserChoiceLatest 记录；它不会调用内部 Hash 函数，
    // 也不会尝试写入任何关联键。OS build 只能选择旧算法时代，不能单独证明 Latest 已启用。
    const auto legacy = crypto::SelectHashVersion(version);
    output += L"legacy_hash_profile=" + std::wstring(crypto::HashVersionName(legacy)) + L"\n";
    if (association::IsProtocol(specification.extension)) {
        output += L"hash_profile=ProtocolUserChoice\n";
        output += L"hash_profile_reason=UserChoiceLatest detection is limited to file extensions\n";
        return;
    }

    const std::wstring protected_path =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SystemProtectedUserData\\" +
        std::wstring(sid) + L"\\AnyoneRead\\AppDefaults";
    DWORD hash_version = 0;
    DWORD type = 0;
    DWORD size = sizeof(hash_version);
    wil::unique_hkey protected_key;
    const LSTATUS open_status = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, protected_path.c_str(), 0,
        KEY_READ | KEY_WOW64_64KEY, protected_key.put());
    const LSTATUS version_status = open_status == ERROR_SUCCESS
        ? ::RegQueryValueExW(protected_key.get(), L"HashVersion", nullptr, &type,
                              reinterpret_cast<BYTE*>(&hash_version), &size)
        : open_status;
    const std::wstring latest = association::ChoiceParent(specification.extension) + L"\\UserChoiceLatest";
    const auto latest_hash = registry::ReadString(HKEY_CURRENT_USER, latest, L"Hash");
    const auto latest_prog_id = registry::ReadString(HKEY_CURRENT_USER, latest + L"\\ProgId", L"ProgId");
    const auto latest_time = registry::LastWriteTime(HKEY_CURRENT_USER, latest + L"\\ProgId");
    const bool valid_version = version_status == ERROR_SUCCESS && type == REG_DWORD &&
        size == sizeof(hash_version);
    const bool latest_record_present = latest_hash && latest_prog_id && latest_time;
    if (valid_version) {
        output += std::format(L"latest_hash_version={}\n", hash_version);
    } else {
        output += std::format(L"latest_hash_version=unavailable,error={}\n", version_status);
    }
    if (valid_version && hash_version == 1 && latest_record_present) {
        output += L"hash_profile=UserChoiceLatest\n";
        output += L"hash_profile_reason=HashVersion=1 and UserChoiceLatest record is present\n";
    } else if (valid_version && hash_version == 1) {
        output += L"hash_profile=UserChoiceLatestPending\n";
        output += L"hash_profile_reason=HashVersion=1 is present but the Latest record is incomplete\n";
    } else if (latest_record_present) {
        output += L"hash_profile=Unknown\n";
        output += L"hash_profile_reason=UserChoiceLatest record exists without a validated HashVersion=1\n";
    } else {
        output += L"hash_profile=LegacyUserChoice\n";
        output += L"hash_profile_reason=No validated UserChoiceLatest record was found\n";
    }
}

} // namespace

std::wstring Snapshot(const association::Specification& specification, bool query_shell) {
    // 日志时间为 UTC，便于与 UserChoice 的 FILETIME 和分钟截断规则核对。
    // process_bits 是程序位数，native_architecture 是真实系统架构，两者不能混为一谈。
    SYSTEMTIME now{};
    ::GetSystemTime(&now);
    const auto version = platform::GetWindowsVersion();
    const auto sid = platform::GetCurrentUserSid();
    SYSTEM_INFO system{};
    ::GetNativeSystemInfo(&system);
    std::wstring output = std::format(
        L"=== snapshot UTC {:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} ===\n"
        L"build_id=vm-first-use-v5-browser\nWindows={}.{}.{}\nprocess_bits={}\n"
        L"native_architecture={}\nSID={}\nextension={}\nexpected_progid={}\n"
        L"association_toasts_enabled={}\nexperiment_notify={}\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        version.major, version.minor, version.build, sizeof(void*) * 8,
        system.wProcessorArchitecture, sid, specification.extension, specification.prog_id,
        specification.update_association_toasts, specification.experimental_notify);
    DumpHashProfile(output, specification, version, sid);
    output += std::format(L"association_type={}\nreuse_registered={}\n",
        association::IsProtocol(specification.extension) ? L"protocol" : L"file", specification.reuse_registered);
    DumpKey(output, HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion", L"HKLM");
    const std::wstring file_ext = association::ChoiceParent(specification.extension);
    // ext=.pdf 时，下列 HKCU 键为：
    // HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf，
    // 以及该键下的 UserChoice、OpenWithList、OpenWithProgids。
    // HKCR 列表则读取合并视图中的 .pdf 和 .pdf\OpenWithProgids。
    // 协议模式将 file_ext 换为 HKCU\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\https。
    for (const auto& path : {file_ext, file_ext + L"\\UserChoice", file_ext + L"\\OpenWithList",
                             file_ext + L"\\OpenWithProgids"}) {
        DumpKey(output, HKEY_CURRENT_USER, path, L"HKCU");
    }
    if (specification.reuse_registered) {
        // 已安装浏览器可能仅在 HKLM 注册；用合并 HKCR 记录实际命令来源，不能只看 HKCU。
        DumpKey(output, HKEY_CLASSES_ROOT, specification.prog_id + L"\\shell\\open\\command", L"HKCR");
    }
    for (const auto& path : {specification.extension, specification.extension + L"\\OpenWithProgids"}) {
        DumpKey(output, HKEY_CLASSES_ROOT, path, L"HKCR");
    }
    for (const auto& path : {L"Software\\Classes\\" + specification.prog_id,
                             L"Software\\Classes\\" + specification.prog_id + L"\\DefaultIcon",
                             L"Software\\Classes\\" + specification.prog_id + L"\\shell\\open\\command"}) {
        // prog_id=SamplePdfReader.pdf 时依次记录 HKCU\Software\Classes\SamplePdfReader.pdf、
        // HKCU\Software\Classes\SamplePdfReader.pdf\DefaultIcon 和 \shell\open\command。
        DumpKey(output, HKEY_CURRENT_USER, path, L"HKCU");
    }
    DumpKey(output, HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts",
        L"HKCU", L"_" + specification.extension);
    DumpKey(output, HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\Shell\\Associations", L"HKCU");
    const auto prog_id = registry::ReadString(HKEY_CURRENT_USER, file_ext + L"\\UserChoice", L"ProgId");
    // 从 HKCU\...\FileExts\.pdf\UserChoice 读取真实 ProgId，而不是直接代入期望值。
    // 这样日志能判断旧应用的 Hash 本来有效，还是已有记录已经被破坏。
    const auto stored = registry::ReadString(HKEY_CURRENT_USER, file_ext + L"\\UserChoice", L"Hash");
    const auto time = registry::LastWriteTime(HKEY_CURRENT_USER, file_ext + L"\\UserChoice");
    SYSTEMTIME modified{};
    if (prog_id && stored && time && ::FileTimeToSystemTime(&*time, &modified) && !sid.empty()) {
        const auto hash = crypto::GenerateUserChoiceHash(specification.extension, *prog_id, sid, modified, version);
        output += std::format(L"UserChoice.hash_expected={}\nUserChoice.hash_matches={}\n",
                               hash.value, hash.ok && hash.value == *stored);
        if (!hash.ok) {
            output += L"UserChoice.hash_error=" + hash.error + L"\n";
        }
    } else {
        output += L"UserChoice.hash_matches=unavailable\n";
    }
    if (query_shell) {
        DumpShell(output, specification);
    } else {
        output += L"Shell.query=skipped_before_operation\n";
    }
    output += L"=== snapshot end ===\n";
    return output;
}

} // namespace dfa::diagnostics
