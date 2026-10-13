#include "association.h"
#include "registry.h"
#include "diagnostics.h"
#include "platform.h"

#include <windows.h>
#include <wil/resource.h>

#include <cstdio>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void Run() {
    const std::wstring path = L"Software\\DefaultFileAssociationRegistrationTest-" +
        std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
    wil::unique_hkey sandbox;
    DWORD disposition = 0;
    Require(::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                               REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                               sandbox.put(), &disposition) == ERROR_SUCCESS,
            "Create association sandbox");
    Require(disposition == REG_CREATED_NEW_KEY, "Never reuse an existing key");
    auto cleanup = wil::scope_exit([&] {
        sandbox.reset();
        ::RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    });
    // 测试只写进程内重定向的 HKCU，不改变用户真实的默认打开方式。
    Require(::RegOverridePredefKey(HKEY_CURRENT_USER, sandbox.get()) == ERROR_SUCCESS,
            "Redirect test HKCU");
    auto restore = wil::scope_exit([] { ::RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); });

    dfa::association::Specification specification;
    specification.extension = L".dfa-registration-test";
    specification.prog_id = L"Dfa.RegistrationTest";
    specification.executable = L"C:\\Program Files\\中文阅读器\\reader.exe";
    // 此路径只是注册表值测试输入；测试不启动 EXE，也不要求这个示例程序实际存在。
    specification.description = L"中文阅读器";
    specification.icon_index = 1;
    specification.use_hta = false;
    specification.update_association_toasts = false;
    const std::wstring prog_key = L"Software\\Classes\\" + specification.prog_id;
    // 在重定向的 HKCU 内检查 Software\Classes\Dfa.RegistrationTest；不操作真实 PDF 默认。
    const std::wstring capabilities = L"Software\\DefaultFileAssociation\\" +
        specification.prog_id + L"\\Capabilities";
    auto expect_string = [](const std::wstring& key, const std::wstring& name,
                            const std::wstring& expected) {
        const auto value = dfa::registry::ReadString(HKEY_CURRENT_USER, key, name);
        Require(value && *value == expected, "Registered value matches request");
    };
    Require(dfa::association::RegisterCandidate(specification).status == dfa::Status::RegisteredOnly,
            "Register candidate");
    expect_string(prog_key, L"", specification.description);
    expect_string(prog_key + L"\\DefaultIcon", L"",
                  L"\"C:\\Program Files\\中文阅读器\\reader.exe\",1");
    expect_string(prog_key + L"\\shell\\open\\command", L"",
                  L"\"C:\\Program Files\\中文阅读器\\reader.exe\" \"%1\" %*");
    expect_string(L"Software\\RegisteredApplications", specification.prog_id, capabilities);
    expect_string(capabilities, L"ApplicationName", specification.prog_id);
    expect_string(capabilities + L"\\FileAssociations", specification.extension, specification.prog_id);

    specification.description = L"新描述";
    specification.icon_index.reset();
    specification.icon_path = L"C:\\Icons\\document.ico";
    Require(dfa::association::RegisterCandidate(specification).status == dfa::Status::RegisteredOnly,
            "Update existing registration");
    expect_string(prog_key, L"", specification.description);
    expect_string(prog_key + L"\\DefaultIcon", L"", L"\"C:\\Icons\\document.ico\"");
    const std::wstring toasts =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts";
    Require(!dfa::registry::LastWriteTime(HKEY_CURRENT_USER, toasts), "Baseline never writes toast state");
    const std::wstring file_ext =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + specification.extension;
    Require(dfa::registry::SetEmptyValue(HKEY_CURRENT_USER, file_ext + L"\\OpenWithProgids",
                                        L"Dfa.PreviousCandidate").has_value(), "Seed other candidate");
    Require(dfa::registry::SetDword(HKEY_CURRENT_USER, toasts, L"Unrelated_.txt", 7).has_value(),
            "Seed unrelated toast");
    // 第二次注册用全新请求，验证默认行为已补齐确认记录，无需设置实验开关。
    dfa::association::Specification default_request;
    default_request.extension = specification.extension;
    default_request.prog_id = specification.prog_id;
    default_request.executable = specification.executable;
    default_request.description = specification.description;
    default_request.icon_path = specification.icon_path;
    specification = default_request;
    Require(dfa::association::RegisterCandidate(specification).status == dfa::Status::RegisteredOnly,
            "Default registration writes association toasts");
    DWORD value = 42;
    if (dfa::platform::GetWindowsVersion().IsWindows10OrGreater()) {
        for (const auto& name : {specification.prog_id + L"_" + specification.extension,
                                 L"Dfa.PreviousCandidate_" + specification.extension}) {
            DWORD size = sizeof(value);
            Require(::RegGetValueW(HKEY_CURRENT_USER, toasts.c_str(), name.c_str(), RRF_RT_REG_DWORD,
                                   nullptr, &value, &size) == ERROR_SUCCESS && value == 0,
                    "Target and existing candidate toasts are DWORD zero");
        }
    } else {
        const auto names = dfa::registry::ValueNames(HKEY_CURRENT_USER, toasts);
        Require(names && names->size() == 1 && names->front() == L"Unrelated_.txt",
                "Pre-Windows 10 registration does not add toast records");
    }
    DWORD bytes = sizeof(value);
    Require(::RegGetValueW(HKEY_CURRENT_USER, toasts.c_str(), L"Unrelated_.txt", RRF_RT_REG_DWORD,
                           nullptr, &value, &bytes) == ERROR_SUCCESS && value == 7,
            "Other extensions remain unchanged");
    const auto before = dfa::registry::LastWriteTime(HKEY_CURRENT_USER, prog_key);
    const auto snapshot = dfa::diagnostics::Snapshot(specification, false);
    const auto after = dfa::registry::LastWriteTime(HKEY_CURRENT_USER, prog_key);
    Require(before && after && ::CompareFileTime(&*before, &*after) == 0,
            "Diagnostic collection does not modify registered keys");
    Require(snapshot.find(L"UserChoice.hash_matches=unavailable") != std::wstring::npos &&
                snapshot.find(L"Shell.query=skipped_before_operation") != std::wstring::npos,
            "Fresh registration diagnostics distinguish missing choice and skipped Shell query");
    Require(dfa::association::CheckDefault(specification).status != dfa::Status::AlreadyDefault,
            "Registration alone never confirms an effective default");
    wil::unique_hkey values;
    Require(::RegOpenKeyExW(HKEY_CURRENT_USER, file_ext.c_str(), 0, KEY_SET_VALUE,
                            values.put()) == ERROR_SUCCESS, "Open diagnostic value sandbox");
    const BYTE odd[] = {0x41, 0x00, 0x42};
    Require(::RegSetValueExW(values.get(), L"OddUtf16", 0, REG_SZ, odd, sizeof(odd)) == ERROR_SUCCESS,
            "Seed odd-byte string");
    const wchar_t unterminated[] = {L'中', L'文'};
    Require(::RegSetValueExW(values.get(), L"Unterminated", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(unterminated), sizeof(unterminated)) == ERROR_SUCCESS,
            "Seed unterminated UTF-16 string");
    Require(::RegSetValueExW(values.get(), L"ZeroBytes", 0, REG_SZ, nullptr, 0) == ERROR_SUCCESS,
            "Seed zero-byte string");
    const auto malformed = dfa::diagnostics::Snapshot(specification, false);
    Require(malformed.find(L"OddUtf16: type=1, invalid_utf16_byte_count\n") != std::wstring::npos,
            "Malformed UTF-16 produces a diagnostic, not an unbounded read");
    Require(malformed.find(L"Unterminated: type=1, 中文\n") != std::wstring::npos,
            "Unterminated string is decoded by actual byte length");
    Require(malformed.find(L"ZeroBytes: type=1, \n") != std::wstring::npos,
            "Zero-byte string is handled safely");
}

} // namespace

int main() {
    try {
        Run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Association test failure: %s\n", error.what());
        return 1;
    }
}
