#include "latest_native_probe.h"

#include "association_target.h"
#include "platform.h"

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
    if (result.status == dfa::Status::Success) return S_OK;
    return result.win32_error ? HRESULT_FROM_WIN32(result.win32_error) : E_FAIL;
}

HRESULT RunFromEnvironment() {
    const auto mode = EnvironmentValue(L"DFA_HOST_PROBE_MODE");
    const auto prog_id = EnvironmentValue(L"DFA_HOST_PROBE_PROGID");
    std::wstring trace = L"host_executable=" + CurrentHostName() + L"\n";
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
    if (!IsHost(L"InstallUtil.exe")) return E_ACCESSDENIED;
    return RunFromEnvironment();
}
