#include "latest_native_probe.h"

#include "association.h"
#include "association_target.h"
#include "platform.h"
#include "user_choice.h"

#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {

std::wstring EnvironmentValue(const wchar_t* name) {
    std::array<wchar_t, 32768> value{};
    const DWORD count = ::GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (count == 0 || count >= value.size()) return {};
    return value.data();
}

std::wstring CurrentHostName() {
    std::array<wchar_t, 32768> value{};
    const DWORD count = ::GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (count == 0 || count >= value.size()) return {};
    return std::filesystem::path(value.data()).filename().wstring();
}

bool IsLatestKeyPresent() {
    constexpr wchar_t kLatest[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoiceLatest";
    HKEY key = nullptr;
    const auto status = ::RegOpenKeyExW(HKEY_CURRENT_USER, kLatest, 0, KEY_READ, &key);
    if (status == ERROR_SUCCESS) {
        ::RegCloseKey(key);
        return true;
    }
    return false;
}

HRESULT WriteResult(const dfa::Result& result, std::wstring trace) {
    const auto report = EnvironmentValue(L"DFA_HOST_PROBE_REPORT");
    if (report.empty() || !std::filesystem::path(report).is_absolute()) return E_INVALIDARG;
    trace += L"result_message=" + result.message + L"\n";
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, trace.data(), static_cast<int>(trace.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return E_FAIL;
    std::string utf8(static_cast<size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, trace.data(), static_cast<int>(trace.size()),
        utf8.data(), size, nullptr, nullptr) != size) return E_FAIL;
    std::ofstream output(report, std::ios::binary | std::ios::trunc);
    output.write(utf8.data(), size);
    output.close();
    if (!output) return E_FAIL;
    if (result.status == dfa::Status::Success || result.status == dfa::Status::AlreadyDefault) return S_OK;
    return result.win32_error ? HRESULT_FROM_WIN32(result.win32_error) : E_FAIL;
}

HRESULT RunFromEnvironment() {
    const auto mode = EnvironmentValue(L"DFA_HOST_PROBE_MODE");
    const auto prog_id = EnvironmentValue(L"DFA_HOST_PROBE_PROGID");
    const auto algorithm = EnvironmentValue(L"DFA_HOST_PROBE_ALGORITHM");
    std::wstring trace = L"host_executable=" + CurrentHostName() + L"\n";
    trace += L"hash_layout=" + (algorithm.empty() ? L"latest" : algorithm) + L"\n";
    if (algorithm == L"legacy") {
        if (IsLatestKeyPresent()) {
            trace += L"failure_stage=hash_layout\ncompleted_registry_writes=0\nprobe_success=false\n";
            return WriteResult({dfa::Status::Failed,
                L"Legacy UserChoice route was selected while UserChoiceLatest exists.",
                ERROR_REVISION_MISMATCH, false}, std::move(trace));
        }
        if (mode == L"preflight") {
            trace += L"hash_algorithm=published-userchoice\npreflight_success=true\ncompleted_registry_writes=0\n";
            return WriteResult({dfa::Status::Success,
                L"Legacy UserChoice preflight passed; no writes were attempted.", 0, false},
                std::move(trace));
        }
        if ((mode != L"read" && mode != L"write") || !dfa::association::IsValidProgId(prog_id)) {
            return E_INVALIDARG;
        }
        const dfa::user_choice::Request request{L".pdf", prog_id, false};
        dfa::Result result;
        if (mode == L"read") {
            const bool valid = dfa::user_choice::Check(request);
            result = {valid ? dfa::Status::Success : dfa::Status::UserChoiceRejected,
                      valid ? L"Legacy UserChoice and Shell preflight passed." :
                              L"Legacy UserChoice hash did not validate.", 0, false};
            trace += L"completed_registry_writes=0\n";
        } else {
            result = dfa::user_choice::SetDefault(request);
            trace += L"legacy_result_win32_error=" + std::to_wstring(result.win32_error) + L"\n";
            trace += L"legacy_hta_attempted=" + std::wstring(result.hta_attempted ? L"true\n" : L"false\n");
            trace += L"completed_registry_writes=" +
                std::to_wstring(result.status == dfa::Status::Success ? 2u : 0u) + L"\n";
        }
        trace += L"probe_success=" + std::wstring(
            result.status == dfa::Status::Success || result.status == dfa::Status::AlreadyDefault
                ? L"true\n" : L"false\n");
        return WriteResult(result, std::move(trace));
    }
    if (mode == L"preflight") {
        const auto result = dfa::latest_native::Preflight(trace);
        return WriteResult(result, std::move(trace));
    }
    if (mode != L"read" && mode != L"write" || !dfa::association::IsValidProgId(prog_id)) return E_INVALIDARG;
    const auto result = dfa::latest_native::Probe(prog_id, mode == L"write", trace);
    return WriteResult(result, std::move(trace));
}

bool IsHost(std::wstring_view expected) {
    return dfa::platform::EqualsInsensitive(CurrentHostName(), expected);
}

} // namespace

extern "C" HRESULT WINAPI DllRegisterServer() {
    if (!IsHost(L"regsvr32.exe") && !IsHost(L"odbcconf.exe") && !IsHost(L"msiexec.exe")) return E_ACCESSDENIED;
    return RunFromEnvironment();
}

extern "C" HRESULT WINAPI DfaAssociationHostRun() {
    const auto algorithm = EnvironmentValue(L"DFA_HOST_PROBE_ALGORITHM");
    if (!IsHost(L"InstallUtil.exe") && !((algorithm == L"legacy" || algorithm == L"latest") && IsHost(L"MSBuild.exe"))) {
        return E_ACCESSDENIED;
    }
    return RunFromEnvironment();
}
