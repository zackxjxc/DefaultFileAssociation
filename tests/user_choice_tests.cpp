#include "user_choice.h"
#include "platform.h"
#include "registry.h"
#include "diagnostics.h"
#include "crypto/user_choice_hash.h"
#include "hta_workspace.h"

#include <windows.h>
#include <sddl.h>
#include <wil/resource.h>

#include <cstdio>
#include <stdexcept>
#include <fstream>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestHtaWorkspace() {
    // 父目录示例 <Temp>\DfaHtaTest-1234-5678；只创建新目录，不使用任何已有同名目录。
    const auto parent = std::filesystem::temp_directory_path() /
        (L"DfaHtaTest-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64()));
    Require(std::filesystem::create_directory(parent), "Create exclusively owned test directory");
    auto cleanup = wil::scope_exit([&] {
        std::error_code ignored;
        std::filesystem::remove(parent / L"occupied-file", ignored);
        std::filesystem::remove(parent / L"occupied" / L"choice.hta", ignored);
        std::filesystem::remove(parent / L"occupied", ignored);
        std::filesystem::remove(parent, ignored);
    });
    const auto directory = parent / L"中文 HTA";
    {
        dfa::user_choice::HtaWorkspace owner(directory);
        Require(static_cast<bool>(owner), "Create isolated HTA workspace");
        { std::ofstream output(owner.file()); output << "original"; }
        {
            dfa::user_choice::HtaWorkspace collision(directory);
            Require(!collision, "Second workspace refuses the existing directory");
        }
        Require(std::filesystem::exists(owner.file()), "Failed owner never removes another owner's file");
    }
    Require(!std::filesystem::exists(directory), "Owned HTA file and empty directory are cleaned up");
    Require(std::filesystem::create_directory(parent / L"occupied"), "Seed existing directory");
    { std::ofstream output(parent / L"occupied" / L"choice.hta"); output << "keep"; }
    {
        dfa::user_choice::HtaWorkspace collision(parent / L"occupied");
        Require(!collision, "Foreign directory is never reused");
    }
    std::string content;
    { std::ifstream input(parent / L"occupied" / L"choice.hta"); input >> content; }
    Require(content == "keep", "Existing HTA content is neither replaced nor deleted");
    { std::ofstream output(parent / L"occupied-file"); output << "keep"; }
    {
        dfa::user_choice::HtaWorkspace collision(parent / L"occupied-file");
        Require(!collision, "Existing ordinary file cannot become a workspace");
    }
    Require(std::filesystem::is_regular_file(parent / L"occupied-file"), "Foreign file survives cleanup");
}

void Run() {
    // 完整测试根示例 HKCU\Software\DefaultFileAssociationChoiceTest-1234-5678。
    // 重定向后，生产代码访问的 HKCU\Software\... 实际落在这个进程的私人测试根下。
    const std::wstring path = L"Software\\DefaultFileAssociationChoiceTest-" +
        std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
    wil::unique_hkey sandbox;
    DWORD disposition = 0;
    Require(::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                               REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                               sandbox.put(), &disposition) == ERROR_SUCCESS,
            "Create private UserChoice sandbox");
    Require(disposition == REG_CREATED_NEW_KEY, "Never reuse an existing key");
    auto cleanup = wil::scope_exit([&] {
        sandbox.reset();
        ::RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    });
    // HKCU 重定向只在当前测试进程生效，子进程不会继承，因此强制关闭 HTA，避免碰到真实关联。
    Require(::RegOverridePredefKey(HKEY_CURRENT_USER, sandbox.get()) == ERROR_SUCCESS,
            "Redirect test HKCU");
    auto restore = wil::scope_exit([] { ::RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); });
    const dfa::user_choice::Request request{L".dfa-test", L"Dfa.Test", false};
    const std::wstring key =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.dfa-test\\UserChoice";
    Require(dfa::registry::SetString(HKEY_CURRENT_USER, key, L"ProgId", L"Previous.App").has_value(),
            "Seed previous choice");
    Require(dfa::registry::SetString(HKEY_CURRENT_USER, key, L"Hash", L"invalid").has_value(),
            "Seed previous hash");
    const std::wstring sid = dfa::platform::GetCurrentUserSid();
    Require(!sid.empty(), "Current user SID");
    const std::wstring sddl = L"O:" + sid + L"D:(D;;0x2;;;" + sid + L")(A;;KA;;;" + sid + L")";
    wil::unique_hlocal_security_descriptor descriptor;
    Require(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, descriptor.put(), nullptr) != FALSE,
            "Create UserChoice-style ACL");
    wil::unique_hkey choice;
    Require(::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, WRITE_DAC, choice.put()) == ERROR_SUCCESS,
            "Open choice ACL");
    Require(::RegSetKeySecurity(choice.get(), DACL_SECURITY_INFORMATION, descriptor.get()) == ERROR_SUCCESS,
            "Set UserChoice write restriction");
    choice.reset();
    Require(!dfa::registry::SetString(HKEY_CURRENT_USER, key, L"ProgId", request.prog_id),
            "Denied writes are reproduced");
    auto result = dfa::user_choice::SetDefault(request);
    Require(result.status == dfa::Status::Success, "Replace restricted UserChoice");
    Require(!result.hta_attempted, "Test never launches HTA");
    Require(dfa::user_choice::Check(request), "Recomputed hash matches key timestamp");
    dfa::association::Specification specification;
    specification.extension = request.extension;
    specification.prog_id = request.prog_id;
    Require(dfa::diagnostics::Snapshot(specification, false).find(L"UserChoice.hash_matches=true") !=
                std::wstring::npos, "Diagnostic reports a matching hash");
    Require(dfa::user_choice::SetDefault(request).status == dfa::Status::AlreadyDefault,
            "Repeated operation verifies existing choice");
    Require(dfa::registry::SetString(HKEY_CURRENT_USER, key, L"Hash", L"tampered").has_value(),
            "Corrupt test hash");
    Require(!dfa::user_choice::Check(request), "Successful writes alone do not imply a valid choice");
    Require(dfa::diagnostics::Snapshot(specification, false).find(L"UserChoice.hash_matches=false") !=
                std::wstring::npos, "Diagnostic identifies an invalid hash");
    Require(dfa::user_choice::SetDefault(request).status == dfa::Status::Success,
            "Repair invalid test hash");
    if (dfa::platform::GetWindowsVersion().IsWindows10OrGreater()) {
        const auto modified = dfa::registry::LastWriteTime(HKEY_CURRENT_USER, key);
        Require(modified.has_value(), "Read repaired key time");
        ULARGE_INTEGER ticks{};
        ticks.LowPart = modified->dwLowDateTime;
        ticks.HighPart = modified->dwHighDateTime;
        ticks.QuadPart -= 60ULL * 10'000'000;
        FILETIME previous_time{ticks.LowPart, ticks.HighPart};
        SYSTEMTIME previous_minute{};
        Require(::FileTimeToSystemTime(&previous_time, &previous_minute) != FALSE, "Previous minute UTC");
        const auto stale = dfa::crypto::GenerateUserChoiceHash(
            request.extension, request.prog_id, sid, previous_minute, dfa::platform::GetWindowsVersion());
        Require(stale.ok && dfa::registry::SetString(HKEY_CURRENT_USER, key, L"Hash", stale.value).has_value(),
                "Seed a hash from the previous minute");
        Require(!dfa::user_choice::Check(request), "Previous-minute hash is rejected");
        Require(dfa::user_choice::SetDefault(request).status == dfa::Status::Success &&
                    dfa::user_choice::Check(request), "Fresh timestamp repairs a stale hash");
    }
}

} // namespace

int main() {
    try {
        TestHtaWorkspace();
        Run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UserChoice test failure: %s\n", error.what());
        return 1;
    }
}
