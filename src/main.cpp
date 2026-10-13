#include "association.h"
#include "cli/command_line.h"
#include "platform.h"
#include "diagnostics.h"
#include "browser.h"
#include "association_target.h"
#include "latest_native_probe.h"
#include "msbuild_backend.h"

#include <windows.h>
#include <CLI/Encoding.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>

namespace {

constexpr int kExitInvalidArguments = 2;
constexpr int kExitElevated = 3;
constexpr int kExitUserChoiceRejected = 4;
constexpr int kExitOperationFailed = 5;
std::ofstream log_file;

void Log(std::wstring_view text) {
    // 日志统一追加 UTF-8，不写 BOM；每次刷新，让用户能拿到故障发生前的内容。
    if (log_file.is_open()) {
        log_file << CLI::narrow(text);
        log_file.flush();
    }
}

void Print(std::wstring_view text, bool error = false) {
    Log(text);
    const HANDLE output = ::GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    DWORD written = 0;
    if (::GetConsoleMode(output, &mode)) {
        ::WriteConsoleW(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        // 控制台输出 UTF-16，重定向输出 UTF-8，避免中文错误被 CRT 区域设置丢弃。
        const std::string utf8 = CLI::narrow(text);
        ::WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

std::wstring DefaultDescription(const dfa::cli::Options& options,
                                const std::wstring& prog_id) {
    if (!options.description.empty()) {
        return options.description;
    }
    if (!options.executable.empty() && !options.executable.stem().empty()) {
        return options.executable.stem().wstring();
    }
    return prog_id;
}

void PrintResult(const dfa::Result& result) {
    Print(result.message + L"\n");
    if (result.hta_attempted) {
        Print(L"HTA compatibility fallback was attempted.\n");
    }
    if (result.win32_error != ERROR_SUCCESS) {
        Print(std::format(L"Win32 error {}: {}\n", result.win32_error,
                           dfa::platform::FormatWindowsError(result.win32_error)), true);
    }
}

int ResultExitCode(const dfa::Result& result) {
    // 0 表示请求成功，2 为参数错误，3 为进程已提升，4 为系统拒绝默认选择，5 为操作失败。
    // --diagnose 的 0 只表示采集完成，不声明默认关联已生效。
    switch (result.status) {
    case dfa::Status::Success:
    case dfa::Status::AlreadyDefault:
    case dfa::Status::RegisteredOnly:
        return 0;
    case dfa::Status::UserChoiceRejected:
        return kExitUserChoiceRejected;
    case dfa::Status::Failed:
        return kExitOperationFailed;
    }
    return kExitOperationFailed;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // 顺序：解析并校验参数 -> 打开可选日志 -> 拒绝提升进程 -> 执行所选模式。
    const dfa::cli::ParseResult parsed = dfa::cli::Parse(argc, argv);
    if (parsed.show_help) {
        Print(dfa::cli::HelpText());
        return 0;
    }
    if (!parsed.ok) {
        Print(L"Error: " + parsed.error + L"\n\n" + dfa::cli::HelpText(), true);
        return kExitInvalidArguments;
    }
    if (!parsed.options.log_file.empty()) {
        // log_file 是磁盘路径；绝对路径原样使用，相对路径相对于工具的当前工作目录。
        // 日志打开失败立即返回，避免实际已修改关联却没有留下用户请求的诊断记录。
        log_file.open(parsed.options.log_file, std::ios::binary | std::ios::app);
        if (!log_file) {
            Print(L"Unable to open --log-file; no registry changes were made.\n", true);
            return kExitOperationFailed;
        }
        Print(L"\n=== command: " + std::wstring(::GetCommandLineW()) + L" ===\n");
    }

    if (dfa::platform::IsProcessElevated()) {
        Print(L"请以普通用户权限启动此工具。\n", true);
        return kExitElevated;
    }

    const dfa::cli::Options& options = parsed.options;
    const std::wstring& prog_id = options.prog_id;

    if (options.latest_native_probe) {
        std::wstring trace;
        const auto result = dfa::latest_native::Probe(prog_id, !options.check, trace);
        Print(trace);
        PrintResult(result);
        return log_file.is_open() && !log_file ? kExitOperationFailed : ResultExitCode(result);
    }

    dfa::association::Specification specification;
    specification.extension = options.protocol.empty() ? options.extension : options.protocol;
    specification.executable = options.executable;
    specification.prog_id = prog_id;
    specification.icon_path = options.icon_path;
    specification.icon_index = options.icon_index;
    specification.description = DefaultDescription(options, prog_id);
    specification.use_hta = options.use_hta;
    specification.reuse_registered = !options.protocol.empty() || !options.browser.empty();
    specification.update_association_toasts = !options.check && !options.diagnose &&
        (options.experiment == "default" || options.experiment == "toasts" || options.experiment == "toasts-notify");
    specification.experimental_notify = options.experiment == "notify" || options.experiment == "toasts-notify";
    // 普通设置使用 default/toasts；none 用于复现旧行为。所有只读模式都关闭确认记录写入。

    if (options.msbuild && !options.dry_run) {
        std::wstring trace;
        const auto mode = options.preflight ? dfa::msbuild::Mode::Preflight :
            options.check ? dfa::msbuild::Mode::Check : dfa::msbuild::Mode::Set;
        const auto result = dfa::msbuild::Run(specification, mode, trace);
        Print(trace);
        PrintResult(result);
        return log_file.is_open() && !log_file ? kExitOperationFailed : ResultExitCode(result);
    }
    std::vector<dfa::association::Specification> plan{specification};
    if (!options.browser.empty()) {
        auto resolved = dfa::browser::Resolve(options.browser, specification);
        if (!resolved) {
            Print(L"Browser preflight failed: " + dfa::registry::DescribeError(resolved.error()) + L"\n", true);
            return kExitOperationFailed;
        }
        plan = std::move(*resolved);
    }
    // 在任何关联写入前校验所有已安装处理器；dry-run 也不能把缺失 ProgID 当成有效计划。
    for (const auto& request : plan) {
        if (request.reuse_registered) {
            auto command = dfa::association::ReadRegisteredCommand(request.prog_id);
            if (!command) {
                Print(L"Registered handler preflight failed: " + dfa::registry::DescribeError(command.error()) + L"\n", true);
                return kExitOperationFailed;
            }
        }
    }

    if (options.dry_run) {
        if (options.msbuild) Print(L"backend: framework-msbuild (new-format PDF)\n");
        // 只显示已经校验完的最终参数，不注册、不启动阅读器，也不启动 HTA。
        for (const auto& request : plan) {
            if (request.reuse_registered) {
                auto command = dfa::association::ReadRegisteredCommand(request.prog_id);
                if (!command) {
                    Print(L"Registered command changed during preflight.\n", true);
                    return kExitOperationFailed;
                }
                Print(std::format(L"{}: {}\nprog-id: {}\nregistered-command: {}\nuser-choice-key: HKCU\\{}\nHTA fallback: {}\n",
                    dfa::association::IsProtocol(request.extension) ? L"protocol" : L"extension",
                    request.extension, request.prog_id, *command, dfa::association::ChoiceKey(request.extension),
                    request.use_hta ? L"enabled" : L"disabled"));
            } else {
                Print(std::format(L"extension: {}\nexecutable: {}\nprog-id: {}\ndescription: {}\n"
                           L"icon: {}{}\nHTA fallback: {}\nexperiment: {}\nassociation toasts: {}\n",
                           request.extension, request.executable.wstring(),
                           request.prog_id, request.description,
                           request.icon_path.empty() ? request.executable.wstring() : request.icon_path.wstring(),
                           request.icon_index ? L"," + std::to_wstring(*request.icon_index) : L"",
                           request.use_hta ? L"enabled" : L"disabled", CLI::widen(options.experiment),
                           request.update_association_toasts ? L"enabled" : L"disabled"));
            }
        }
        return 0;
    }

    if (options.diagnose) {
        for (const auto& request : plan) {
            Print(dfa::diagnostics::Snapshot(request));
        }
        Print(L"Diagnostic collection completed; this does not confirm a working default.\n");
        return log_file.is_open() && !log_file ? kExitOperationFailed : 0;
    }
    if (log_file.is_open()) {
        // 设置前只读注册表，避免额外 Shell 查询干扰首次打开场景。
        for (const auto& request : plan) {
            Log(L"stage=before\n" + dfa::diagnostics::Snapshot(request, false));
        }
        if (!log_file) {
            Print(L"Log write failed before the operation; no registry changes were made.\n", true);
            return kExitOperationFailed;
        }
    }
    int exit_code = 0;
    for (const auto& request : plan) {
        Print(L"[" + request.extension + L"] " + request.prog_id + L"\n");
        const dfa::Result result = options.check
            ? dfa::association::CheckDefault(request)
            : dfa::association::SetDefault(request);
        PrintResult(result);
        // 批量设置不具备事务语义；继续报告每一项，任何失败都使整体返回非零。
        const int item_exit_code = ResultExitCode(result);
        exit_code = std::max(exit_code, item_exit_code);
        if (log_file.is_open()) {
            // 写后快照包含 Shell 查询；日志写失败不撤销已经完成的关联操作，返回值如实报告失败。
            Log(std::format(L"operation_exit_code={}\nstage=after\n", item_exit_code) +
                dfa::diagnostics::Snapshot(request));
            if (!log_file) {
                Print(L"Log write failed; the association operation may already have completed.\n", true);
                return kExitOperationFailed;
            }
        }
    }
    if (plan.size() > 1) {
        Print(exit_code == 0 ? L"All browser associations verified.\n" :
            L"Browser setup was incomplete; see the per-association results.\n");
    }
    return log_file.is_open() && !log_file ? kExitOperationFailed : exit_code;
}
