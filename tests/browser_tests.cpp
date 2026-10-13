#include "browser.h"
#include "association_target.h"
#include "diagnostics.h"
#include "user_choice.h"

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
    const auto path = L"Software\\DfaBrowserTest-" + std::to_wstring(::GetCurrentProcessId()) +
        L"-" + std::to_wstring(::GetTickCount64());
    wil::unique_hkey sandbox;
    DWORD disposition = 0;
    Require(::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                            nullptr, sandbox.put(), &disposition) == ERROR_SUCCESS &&
            disposition == REG_CREATED_NEW_KEY, "Create a new browser sandbox");
    auto cleanup = wil::scope_exit([&] { sandbox.reset(); ::RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); });
    wil::unique_hkey user, machine, classes;
    auto create = [&](const wchar_t* name, wil::unique_hkey& key) {
        Require(::RegCreateKeyExW(sandbox.get(), name, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                                 key.put(), nullptr) == ERROR_SUCCESS, "Create private root");
    };
    create(L"User", user);
    create(L"Machine", machine);
    create(L"Classes", classes);
    Require(::RegOverridePredefKey(HKEY_CURRENT_USER, user.get()) == ERROR_SUCCESS, "Redirect HKCU");
    auto restore_user = wil::scope_exit([] { ::RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); });
    Require(::RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine.get()) == ERROR_SUCCESS, "Redirect HKLM");
    auto restore_machine = wil::scope_exit([] { ::RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr); });
    Require(::RegOverridePredefKey(HKEY_CLASSES_ROOT, classes.get()) == ERROR_SUCCESS, "Redirect HKCR");
    auto restore_classes = wil::scope_exit([] { ::RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr); });
    // 三个根仅在当前测试进程中重定向；不改变真实浏览器、不打开 URL、也不启动 HTA。
    auto write = [](HKEY root, std::wstring_view key, std::wstring_view name, std::wstring_view value) {
        Require(dfa::registry::SetString(root, key, name, value).has_value(), "Seed registration");
    };
    const std::wstring capabilities = L"Software\\Clients\\StartMenuInternet\\TestBrowser\\Capabilities";
    const std::wstring url_command = L"\"C:\\Apps\\Browser\\browser.exe\" -osint -url \"%1\"";
    const std::wstring html_command = L"\"C:\\Apps\\Browser\\browser.exe\" -osint -file \"%1\"";
    write(HKEY_CLASSES_ROOT, L"Test.Url\\shell\\open\\command", L"", url_command);
    write(HKEY_CLASSES_ROOT, L"Test.Html\\shell\\open\\command", L"", html_command);
    auto seed = [&](HKEY root, const wchar_t* name, const wchar_t* url_id) {
        write(root, L"Software\\RegisteredApplications", name, capabilities);
        for (const auto* protocol : {L"http", L"https"}) {
            write(root, capabilities + L"\\URLAssociations", protocol, url_id);
        }
        for (const auto* extension : {L".htm", L".html"}) {
            write(root, capabilities + L"\\FileAssociations", extension, L"Test.Html");
        }
    };
    seed(HKEY_LOCAL_MACHINE, L"Test Browser", L"Test.Url");
    dfa::association::Specification defaults;
    defaults.use_hta = false;
    defaults.update_association_toasts = false;
    auto plan = dfa::browser::Resolve(L"Test Browser", defaults);
    Require(plan && plan->size() == 4, "Resolve machine browser into all four associations");
    Require((*plan)[0].extension == L"http" && (*plan)[1].extension == L"https" &&
            (*plan)[2].extension == L".htm" && (*plan)[3].extension == L".html", "Stable batch order");
    Require((*plan)[0].prog_id == L"Test.Url" && (*plan)[3].prog_id == L"Test.Html",
            "URL and HTML use separate installed ProgIDs");
    Require((*plan)[0].reuse_registered && !(*plan)[0].use_hta, "Reuse flags survive planning");
    auto command = dfa::association::ReadRegisteredCommand(L"Test.Url");
    Require(command && *command == url_command, "Installed browser arguments are preserved verbatim");
    const auto protocol_request = (*plan)[1];
    Require(dfa::association::RegisterCandidate(protocol_request).status == dfa::Status::RegisteredOnly,
            "Reuse existing protocol registration");
    Require(dfa::user_choice::SetDefault({L"https", L"Test.Url", false}).status == dfa::Status::Success,
            "Write protocol UserChoice in sandbox");
    const auto choice = dfa::association::ChoiceKey(L"https");
    auto prog_id = dfa::registry::ReadString(HKEY_CURRENT_USER, choice, L"ProgId");
    Require(prog_id && *prog_id == L"Test.Url", "Protocol UserChoice uses UrlAssociations");
    Require(!dfa::registry::LastWriteTime(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\https"),
            "Never create a FileExts key for a protocol");
    Require(dfa::user_choice::Check({L"https", L"Test.Url", false}), "Protocol hash uses https without a dot");
    Require(dfa::diagnostics::Snapshot(protocol_request, false).find(L"UserChoice.hash_matches=true") !=
            std::wstring::npos, "Protocol diagnostic reads the same URL UserChoice");
    auto unchanged = dfa::association::ReadRegisteredCommand(L"Test.Url");
    Require(unchanged && *unchanged == url_command, "Setting default does not replace browser open command");
    seed(HKEY_CURRENT_USER, L"Test Browser", L"Test.Html");
    auto user_plan = dfa::browser::Resolve(L"Test Browser", defaults);
    Require(user_plan && (*user_plan)[0].prog_id == L"Test.Html", "User browser registration takes precedence");
    write(HKEY_CURRENT_USER, capabilities + L"\\FileAssociations", L".html", L"Test.Missing");
    Require(!dfa::browser::Resolve(L"Test Browser", defaults), "Missing last handler rejects the complete plan");
    Require(dfa::user_choice::Check({L"https", L"Test.Url", false}), "Failed preflight leaves current choice unchanged");
    Require(!dfa::browser::Resolve(L"Not Installed", defaults), "Unknown browser is rejected");
}

} // namespace

int main() {
    try { Run(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Browser test failure: %s\n", error.what());
        return 1;
    }
}
