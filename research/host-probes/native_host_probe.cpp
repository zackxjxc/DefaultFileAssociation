// Authored by Codex. Controlled, build-specific current-user PDF experiment.
#include "latest_native_probe.h"
#include "platform.h"

#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" HRESULT WINAPI DllRegisterServer() {
    try {
        std::array<wchar_t, 32768> host{}, report{}, mode{};
        if (!::GetModuleFileNameW(nullptr, host.data(), static_cast<DWORD>(host.size())) ||
            !::GetEnvironmentVariableW(L"DFA_HOST_PROBE_REPORT", report.data(), static_cast<DWORD>(report.size())) ||
            !::GetEnvironmentVariableW(L"DFA_HOST_PROBE_MODE", mode.data(), static_cast<DWORD>(mode.size()))) {
            return E_INVALIDARG;
        }
        const auto name = std::filesystem::path(host.data()).filename().wstring();
        if (!dfa::platform::EqualsInsensitive(name, L"odbcconf.exe") &&
            !dfa::platform::EqualsInsensitive(name, L"regsvr32.exe") &&
            !dfa::platform::EqualsInsensitive(name, L"msiexec.exe")) return E_ACCESSDENIED;
        const std::wstring operation(mode.data());
        if (operation != L"read" && operation != L"write") return E_INVALIDARG;
        const std::filesystem::path log_path(report.data());
        if (!log_path.is_absolute() || log_path.parent_path().filename() != L"host-probes") return E_INVALIDARG;
        std::wstring trace;
        const auto result = dfa::latest_native::Probe(L"samplepdfreader.pdf", operation == L"write", trace);
        trace = L"host_executable=" + std::wstring(host.data()) + L"\n" + trace;
        trace += L"result_message=" + result.message + L"\n";
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, trace.data(), static_cast<int>(trace.size()),
            nullptr, 0, nullptr, nullptr);
        if (size <= 0) return E_FAIL;
        std::string utf8(static_cast<size_t>(size), '\0');
        if (::WideCharToMultiByte(CP_UTF8, 0, trace.data(), static_cast<int>(trace.size()),
            utf8.data(), size, nullptr, nullptr) != size) return E_FAIL;
        std::ofstream output(log_path, std::ios::binary | std::ios::trunc);
        output.write(utf8.data(), size);
        output.close();
        if (!output) return E_FAIL;
        if (result.status == dfa::Status::Success) return S_OK;
        return result.win32_error ? HRESULT_FROM_WIN32(result.win32_error) : E_FAIL;
    } catch (...) {
        return E_FAIL;
    }
}
