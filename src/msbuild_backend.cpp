#include "msbuild_backend.h"
#include "msbuild_task_text.h"
#include "platform.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <fstream>
#include <format>
#include <iterator>
#include <vector>

namespace dfa::msbuild {
namespace {
class Workspace {
public:
    std::filesystem::path directory, project, report, console;
    Workspace() {
        std::array<wchar_t, MAX_PATH> temporary{};
        const auto count = ::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        THROW_LAST_ERROR_IF(count == 0);
        THROW_HR_IF(E_UNEXPECTED, count >= temporary.size());
        GUID id{};
        THROW_IF_FAILED(::CoCreateGuid(&id));
        std::array<wchar_t, 40> text{};
        THROW_HR_IF(E_UNEXPECTED, !::StringFromGUID2(id, text.data(), static_cast<int>(text.size())));
        directory = std::filesystem::path(temporary.data()) / (L"dfa-msbuild-" + std::wstring(text.data()));
        THROW_IF_WIN32_BOOL_FALSE(::CreateDirectoryW(directory.c_str(), nullptr));
        owned_ = true;
        project = directory / L"apply.proj";
        report = directory / L"report.json";
        console = directory / L"console.log";
    }
    ~Workspace() {
        if (!owned_) return;
        std::error_code ignored;
        // Remove only our three artifacts and the empty directory, never recurse.
        for (const auto& file : {project, report, console}) std::filesystem::remove(file, ignored);
        std::filesystem::remove(directory, ignored);
    }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
private:
    bool owned_ = false;
};

std::wstring ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    if (bytes.empty()) return {};
    const auto size = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), result.data(), size);
    return result;
}

std::vector<wchar_t> Environment(std::wstring_view mode, std::wstring_view prog_id,
                                  const std::filesystem::path& report) {
    wil::unique_environstrings_ptr environment(::GetEnvironmentStringsW());
    THROW_LAST_ERROR_IF(environment == nullptr);
    std::vector<std::wstring> values;
    for (const wchar_t* entry = environment.get(); *entry; entry += std::wcslen(entry) + 1) {
        if (_wcsnicmp(entry, L"DFA_MSB_", 8) != 0) values.emplace_back(entry);
    }
    values.emplace_back(L"DFA_MSB_MODE=" + std::wstring(mode));
    values.emplace_back(L"DFA_MSB_PROGID=" + std::wstring(prog_id));
    values.emplace_back(L"DFA_MSB_REPORT=" + report.wstring());
    std::ranges::sort(values, [](const auto& left, const auto& right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& value : values) {
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

DWORD Invoke(std::wstring_view prog_id, std::wstring_view mode, std::wstring& trace) {
    std::array<wchar_t, MAX_PATH> windows{};
    const auto count = ::GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    THROW_LAST_ERROR_IF(count == 0);
    THROW_HR_IF(E_UNEXPECTED, count >= windows.size());
    const auto host = std::filesystem::path(windows.data()) / L"Microsoft.NET/Framework64/v4.0.30319/MSBuild.exe";
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !std::filesystem::is_regular_file(host));
    Workspace workspace;
    {
        std::ofstream project(workspace.project, std::ios::binary);
        project.write(kTaskProject, sizeof(kTaskProject) - 1);
        project.close();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), !project);
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
    auto command = platform::QuoteCommandLineArgument(host.wstring()) + L" " +
        platform::QuoteCommandLineArgument(workspace.project.wstring()) + L" /ToolsVersion:4.0 /nologo /v:quiet /nodeReuse:false";
    auto environment = Environment(mode, prog_id, workspace.report);
    wil::unique_handle job(::CreateJobObjectW(nullptr, nullptr));
    THROW_LAST_ERROR_IF(job == nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    THROW_IF_WIN32_BOOL_FALSE(::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
    PROCESS_INFORMATION created{};
    THROW_IF_WIN32_BOOL_FALSE(::CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
        environment.data(), workspace.directory.c_str(), &startup.StartupInfo, &created));
    wil::unique_handle process(created.hProcess), thread(created.hThread);
    if (!::AssignProcessToJobObject(job.get(), process.get())) {
        const auto error = ::GetLastError();
        ::TerminateProcess(process.get(), error);
        ::WaitForSingleObject(process.get(), 5000);
        THROW_WIN32(error);
    }
    if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) THROW_LAST_ERROR();
    const auto wait = ::WaitForSingleObject(process.get(), 60000);
    if (wait != WAIT_OBJECT_0) {
        ::TerminateJobObject(job.get(), ERROR_TIMEOUT);
        ::WaitForSingleObject(process.get(), 5000);
    }
    output.reset();
    DWORD code = ERROR_TIMEOUT;
    if (wait == WAIT_OBJECT_0) THROW_IF_WIN32_BOOL_FALSE(::GetExitCodeProcess(process.get(), &code));
    trace += std::format(L"msbuild_host={}\nmsbuild_mode={}\nmsbuild_exit_code={}\n", host.wstring(), mode, code);
    trace += L"msbuild_console:\n" + ReadText(workspace.console);
    trace += L"msbuild_report:\n" + ReadText(workspace.report);
    return code;
}

Result VerifyShell(const association::Specification& request, std::wstring& trace) {
    auto expected = association::ReadRegisteredCommand(request.prog_id);
    THROW_IF_WIN32_ERROR(expected ? ERROR_SUCCESS : expected.error().status);
    const auto hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wil::unique_couninitialize_call cleanup(SUCCEEDED(hr));
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) THROW_IF_FAILED(hr);
    auto registration = wil::CoCreateInstance<IApplicationAssociationRegistration>(__uuidof(ApplicationAssociationRegistration));
    wil::unique_cotaskmem_string id;
    THROW_IF_FAILED(registration->QueryCurrentDefault(L".pdf", AT_FILEEXTENSION, AL_EFFECTIVE, id.put()));
    DWORD size = 0;
    THROW_IF_FAILED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, L".pdf", L"open", nullptr, &size));
    THROW_HR_IF(E_UNEXPECTED, size == 0);
    std::wstring command(size, L'\0');
    THROW_IF_FAILED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, L".pdf", L"open", command.data(), &size));
    command.resize(std::wcslen(command.c_str()));
    const bool matches = platform::EqualsInsensitive(id.get(), request.prog_id) && platform::EqualsInsensitive(command, *expected);
    trace += std::format(L"shell_prog_id={}\nshell_open_command={}\nshell_matches={}\n", id.get(), command, matches);
    return {matches ? Status::Success : Status::UserChoiceRejected,
        matches ? L"New PDF Hash and effective Shell handler verified." : L"Shell did not accept the requested PDF association.", 0, false};
}
}

Result Run(const association::Specification& request, Mode mode, std::wstring& trace) {
    trace = L"engine=framework-msbuild\n";
    try {
        THROW_HR_IF(E_INVALIDARG, request.extension != L".pdf" || request.prog_id.empty());
        THROW_HR_IF(E_ACCESSDENIED, platform::IsProcessElevated());
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), !platform::Is64BitWindows() || !platform::GetWindowsVersion().IsWindows10OrGreater());
        if (mode != Mode::Check) {
            if (Invoke(request.prog_id, L"preflight", trace) != 0)
                return {Status::Failed, L"MSBuild compatibility preflight failed; registration and default were not changed.", ERROR_REVISION_MISMATCH, false};
            if (mode == Mode::Preflight) return {Status::Success, L"MSBuild compatibility preflight passed; no writes were attempted.", 0, false};
            const auto registered = association::RegisterCandidate(request);
            if (registered.status == Status::Failed) return registered;
        }
        const auto code = Invoke(request.prog_id, mode == Mode::Set ? L"set" : L"check", trace);
        if (code != 0) return {Status::Failed, L"MSBuild PDF task failed; see the failure stage in the report. Registration may already exist.", code, false};
        return VerifyShell(request, trace);
    } catch (const wil::ResultException& error) {
        const auto hr = error.GetErrorCode();
        trace += std::format(L"backend_hresult=0x{:08x}\n", static_cast<DWORD>(hr));
        const auto code = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? static_cast<DWORD>(HRESULT_CODE(hr)) : 0;
        return {Status::Failed, L"Framework MSBuild backend failed before completing verification.", code, false};
    } catch (const std::exception&) {
        return {Status::Failed, L"Framework MSBuild backend encountered a file or conversion error.", 0, false};
    }
}
}
