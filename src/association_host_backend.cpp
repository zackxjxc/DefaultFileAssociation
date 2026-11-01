#include "association_host_backend.h"

#include "platform.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <array>
#include <cwchar>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace dfa::association_host {
namespace {

struct Workspace {
    std::filesystem::path directory;
    std::filesystem::path report;
    std::filesystem::path console;
    std::filesystem::path response;

    Workspace() {
        std::array<wchar_t, MAX_PATH> temporary{};
        const auto count = ::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        THROW_LAST_ERROR_IF(count == 0);
        THROW_HR_IF(E_UNEXPECTED, count >= temporary.size());
        GUID id{};
        THROW_IF_FAILED(::CoCreateGuid(&id));
        std::array<wchar_t, 40> text{};
        THROW_HR_IF(E_UNEXPECTED, !::StringFromGUID2(id, text.data(), static_cast<int>(text.size())));
        directory = std::filesystem::path(temporary.data()) / (L"dfa-host-" + std::wstring(text.data()));
        THROW_IF_WIN32_BOOL_FALSE(::CreateDirectoryW(directory.c_str(), nullptr));
        report = directory / L"report.log";
        console = directory / L"console.log";
        response = directory / L"odbc.rsp";
    }

    ~Workspace() {
        std::error_code ignored;
        for (const auto& file : {report, console, response}) std::filesystem::remove(file, ignored);
        std::filesystem::remove(directory, ignored);
    }

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
};

std::wstring ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    if (bytes.empty()) return {};
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int size = ::MultiByteToWideChar(code_page, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0) {
        code_page = CP_ACP;
        flags = 0;
        size = ::MultiByteToWideChar(code_page, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    if (::MultiByteToWideChar(code_page, flags, bytes.data(), static_cast<int>(bytes.size()), result.data(), size) <= 0) {
        return {};
    }
    return result;
}

enum class HashLayout {
    Legacy,
    Latest,
};

HashLayout DetectHashLayout() noexcept {
    constexpr wchar_t kLatest[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoiceLatest";
    HKEY key = nullptr;
    const auto status = ::RegOpenKeyExW(HKEY_CURRENT_USER, kLatest, 0, KEY_READ, &key);
    if (status == ERROR_SUCCESS) {
        ::RegCloseKey(key);
        return HashLayout::Latest;
    }
    return HashLayout::Legacy;
}

std::wstring_view HashLayoutName(HashLayout layout) noexcept {
    return layout == HashLayout::Latest ? L"latest" : L"legacy";
}

std::vector<wchar_t> Environment(std::wstring_view mode, std::wstring_view prog_id,
                                 const std::filesystem::path& report,
                                 const std::filesystem::path& native_dll,
                                 HashLayout layout) {
    wil::unique_environstrings_ptr current(::GetEnvironmentStringsW());
    THROW_LAST_ERROR_IF(current == nullptr);
    std::vector<std::wstring> values;
    for (const wchar_t* entry = current.get(); *entry; entry += std::wcslen(entry) + 1) {
        if (_wcsnicmp(entry, L"DFA_HOST_", 9) != 0) values.emplace_back(entry);
    }
    values.emplace_back(L"DFA_HOST_PROBE_MODE=" + std::wstring(mode));
    values.emplace_back(L"DFA_HOST_PROBE_PROGID=" + std::wstring(prog_id));
    values.emplace_back(L"DFA_HOST_PROBE_REPORT=" + report.wstring());
    values.emplace_back(L"DFA_HOST_PROBE_DLL=" + native_dll.wstring());
    values.emplace_back(L"DFA_HOST_PROBE_ALGORITHM=" + std::wstring(HashLayoutName(layout)));
    std::ranges::sort(values, [](const auto& left, const auto& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });
    std::vector<wchar_t> block;
    for (const auto& value : values) {
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::filesystem::path CurrentExecutableDirectory() {
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD count = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    THROW_LAST_ERROR_IF(count == 0 || count >= path.size());
    return std::filesystem::path(path.data()).parent_path();
}

std::filesystem::path WindowsPath(std::wstring_view relative) {
    std::array<wchar_t, MAX_PATH> directory{};
    const UINT count = ::GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    THROW_LAST_ERROR_IF(count == 0);
    THROW_HR_IF(E_UNEXPECTED, count >= directory.size());
    return std::filesystem::path(directory.data()) / relative;
}

struct HostFiles {
    std::filesystem::path executable;
    std::filesystem::path native_dll;
    std::filesystem::path managed_dll;
};

HostFiles Files(Host host) {
    const auto suffix = sizeof(void*) == 8 ? L"x64" : L"x86";
    const auto directory = CurrentExecutableDirectory();
    const auto native_dll = directory / (L"DfaAssociationHost-" + std::wstring(suffix) + L".dll");
    const auto managed_dll = directory / (L"DfaAssociationManagedHost-" + std::wstring(suffix) + L".dll");
    switch (host) {
    case Host::InstallUtil:
        return {WindowsPath(sizeof(void*) == 8 ? L"Microsoft.NET\\Framework64\\v4.0.30319\\InstallUtil.exe" :
                                                         L"Microsoft.NET\\Framework\\v4.0.30319\\InstallUtil.exe"),
                native_dll, managed_dll};
    case Host::Regsvr32:
        return {WindowsPath(L"System32\\regsvr32.exe"), native_dll, managed_dll};
    case Host::OdbcConf:
        return {WindowsPath(L"System32\\odbcconf.exe"), native_dll, managed_dll};
    case Host::MsiExec:
        return {WindowsPath(L"System32\\msiexec.exe"), native_dll, managed_dll};
    }
    THROW_HR(E_INVALIDARG);
}

struct Invocation {
    DWORD exit_code = ERROR_GEN_FAILURE;
    bool callback_success = false;
};

DWORD FailureCode(DWORD exit_code) noexcept {
    if (exit_code == 0) return ERROR_SUCCESS;
    if (exit_code == ERROR_TIMEOUT) return ERROR_TIMEOUT;
    // Host processes may return HRESULTs or signed values outside the Win32
    // error range; do not expose those values as misleading Win32 messages.
    return ERROR_GEN_FAILURE;
}

Invocation Invoke(Host host, Mode mode, std::wstring_view prog_id, HashLayout layout,
                  std::wstring& trace) {
    const auto files = Files(host);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !std::filesystem::is_regular_file(files.executable));
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !std::filesystem::is_regular_file(files.native_dll));
    if (host == Host::InstallUtil) {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !std::filesystem::is_regular_file(files.managed_dll));
    }

    Workspace workspace;
    if (host == Host::OdbcConf) {
        std::ofstream response(workspace.response, std::ios::binary);
        response << "REGSVR \"" << files.native_dll.string() << "\"\n";
        response.close();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), !response);
    }
    const auto operation = mode == Mode::Preflight ? L"preflight" : mode == Mode::Set ? L"write" : L"read";
    auto environment = Environment(operation, prog_id, workspace.report, files.native_dll, layout);
    std::wstring command = platform::QuoteCommandLineArgument(files.executable.wstring());
    switch (host) {
    case Host::InstallUtil:
        command += L" /LogFile= /LogToConsole=false /U " + platform::QuoteCommandLineArgument(files.managed_dll.wstring());
        break;
    case Host::Regsvr32:
        command += L" /s " + platform::QuoteCommandLineArgument(files.native_dll.wstring());
        break;
    case Host::OdbcConf:
        command += L" /S /F " + platform::QuoteCommandLineArgument(workspace.response.wstring());
        break;
    case Host::MsiExec:
        command += L" /y " + platform::QuoteCommandLineArgument(files.native_dll.wstring());
        break;
    }

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    wil::unique_hfile output(::CreateFileW(workspace.console.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    THROW_LAST_ERROR_IF(output.get() == INVALID_HANDLE_VALUE);
    wil::unique_hfile input(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, 0, nullptr));
    THROW_LAST_ERROR_IF(input.get() == INVALID_HANDLE_VALUE);
    SIZE_T attribute_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<BYTE> attribute_buffer(attribute_size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    THROW_IF_WIN32_BOOL_FALSE(::InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size));
    auto attribute_cleanup = wil::scope_exit([&] { ::DeleteProcThreadAttributeList(attributes); });
    HANDLE handles[]{output.get(), input.get()};
    THROW_IF_WIN32_BOOL_FALSE(::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof(handles), nullptr, nullptr));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output.get();
    startup.StartupInfo.hStdInput = input.get();
    startup.lpAttributeList = attributes;
    wil::unique_handle job(::CreateJobObjectW(nullptr, nullptr));
    THROW_LAST_ERROR_IF(job == nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    THROW_IF_WIN32_BOOL_FALSE(::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
        &limits, sizeof(limits)));
    PROCESS_INFORMATION created{};
    THROW_IF_WIN32_BOOL_FALSE(::CreateProcessW(files.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
        environment.data(), workspace.directory.c_str(), &startup.StartupInfo, &created));
    wil::unique_handle process(created.hProcess), thread(created.hThread);
    if (!::AssignProcessToJobObject(job.get(), process.get())) {
        const auto error = ::GetLastError();
        ::TerminateProcess(process.get(), error);
        ::WaitForSingleObject(process.get(), 5000);
        THROW_WIN32(error);
    }
    THROW_HR_IF(E_FAIL, ::ResumeThread(thread.get()) == static_cast<DWORD>(-1));
    const auto wait = ::WaitForSingleObject(process.get(), 60000);
    if (wait != WAIT_OBJECT_0) {
        ::TerminateJobObject(job.get(), ERROR_TIMEOUT);
        ::WaitForSingleObject(process.get(), 5000);
    }
    output.reset();
    Invocation result;
    if (wait == WAIT_OBJECT_0) THROW_IF_WIN32_BOOL_FALSE(::GetExitCodeProcess(process.get(), &result.exit_code));
    trace += std::format(L"host_executable={}\nhost_mode={}\nhost_exit_code={}\n",
        files.executable.wstring(), operation, FailureCode(result.exit_code));
    const auto report = ReadText(workspace.report);
    trace += L"host_report:\n" + report;
    trace += L"host_console:\n" + ReadText(workspace.console);
    result.callback_success = mode == Mode::Preflight ? report.find(L"preflight_success=true") != std::wstring::npos :
        report.find(L"probe_success=true") != std::wstring::npos;
    return result;
}

Result VerifyShell(const association::Specification& request, std::wstring& trace) {
    const auto expected = association::ReadRegisteredCommand(request.prog_id);
    THROW_IF_WIN32_ERROR(expected ? ERROR_SUCCESS : expected.error().status);
    const auto initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wil::unique_couninitialize_call cleanup(SUCCEEDED(initialized));
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) THROW_IF_FAILED(initialized);
    auto registration = wil::CoCreateInstance<IApplicationAssociationRegistration>(__uuidof(ApplicationAssociationRegistration));
    wil::unique_cotaskmem_string id;
    THROW_IF_FAILED(registration->QueryCurrentDefault(L".pdf", AT_FILEEXTENSION, AL_EFFECTIVE, id.put()));
    DWORD size = 0;
    THROW_IF_FAILED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, L".pdf", L"open", nullptr, &size));
    THROW_HR_IF(E_UNEXPECTED, size == 0);
    std::wstring command(size, L'\0');
    THROW_IF_FAILED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, L".pdf", L"open", command.data(), &size));
    command.resize(std::wcslen(command.c_str()));
    const bool matches = platform::EqualsInsensitive(id.get(), request.prog_id) &&
        platform::EqualsInsensitive(command, *expected);
    trace += std::format(L"shell_prog_id={}\nshell_open_command={}\nshell_matches={}\n", id.get(), command, matches);
    return {matches ? Status::Success : Status::UserChoiceRejected,
        matches ? L"Selected host wrote and Shell verification succeeded." :
                  L"Shell did not accept the requested PDF association.", 0, false};
}

} // namespace

std::optional<Host> ParseHost(std::wstring_view name) noexcept {
    if (platform::EqualsInsensitive(name, L"installutil")) return Host::InstallUtil;
    if (platform::EqualsInsensitive(name, L"regsvr32")) return Host::Regsvr32;
    if (platform::EqualsInsensitive(name, L"odbcconf")) return Host::OdbcConf;
    if (platform::EqualsInsensitive(name, L"msiexec")) return Host::MsiExec;
    return std::nullopt;
}

std::wstring_view HostName(Host host) noexcept {
    switch (host) {
    case Host::InstallUtil: return L"installutil";
    case Host::Regsvr32: return L"regsvr32";
    case Host::OdbcConf: return L"odbcconf";
    case Host::MsiExec: return L"msiexec";
    }
    return L"unknown";
}

Result Run(const association::Specification& request, Host host, Mode mode, std::wstring& trace) {
    trace = std::format(L"engine=association-host\nhost={}\n", HostName(host));
    try {
        THROW_HR_IF(E_INVALIDARG, request.extension != L".pdf" || request.prog_id.empty());
        THROW_HR_IF(E_ACCESSDENIED, platform::IsProcessElevated());
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), !platform::Is64BitWindows() || sizeof(void*) != 8);
        const auto layout = DetectHashLayout();
        trace += L"hash_layout=" + std::wstring(HashLayoutName(layout)) + L"\n";
        if (mode != Mode::Check) {
            const auto preflight = Invoke(host, Mode::Preflight, request.prog_id, layout, trace);
            if (preflight.exit_code != 0 || !preflight.callback_success) {
                return {Status::Failed, L"Selected association host preflight failed; registration and default were not changed.",
                    FailureCode(preflight.exit_code), false};
            }
            if (mode == Mode::Preflight) {
                return {Status::Success, L"Selected association host preflight passed; no writes were attempted.", 0, false};
            }
            const auto registered = association::RegisterCandidate(request);
            if (registered.status == Status::Failed) return registered;
        }
        const auto invocation = Invoke(host, mode == Mode::Set ? Mode::Set : Mode::Check,
                                       request.prog_id, layout, trace);
        if (invocation.exit_code != 0 || !invocation.callback_success) {
            return {Status::Failed, L"Selected association host failed; see the host report.", FailureCode(invocation.exit_code), false};
        }
        return VerifyShell(request, trace);
    } catch (const wil::ResultException& error) {
        const auto hr = error.GetErrorCode();
        trace += std::format(L"backend_hresult=0x{:08x}\n", static_cast<DWORD>(hr));
        const auto code = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? static_cast<DWORD>(HRESULT_CODE(hr)) : 0;
        return {Status::Failed, L"Selected association host backend failed before completing verification.", code, false};
    } catch (const std::exception&) {
        return {Status::Failed, L"Selected association host backend encountered a file or process error.", 0, false};
    }
}

} // namespace dfa::association_host
