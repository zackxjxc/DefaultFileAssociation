#include "cli/command_line.h"
#include "platform.h"
#include "association_target.h"

#include <CLI/CLI.hpp>
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <exception>
#include <string_view>
#include <utility>
#include <vector>

namespace dfa::cli {
namespace {

struct ParserData {
    std::string extension, protocol, browser, executable, icon_path, prog_id, description;
    int icon_index = 0;
    bool no_hta = false;
    bool dry_run = false;
    bool check = false;
    bool diagnose = false;
    bool latest_native_probe = false;
    std::string experiment = "default";
    std::string log_file;
};

void Configure(CLI::App& app, ParserData& data) {
    // 横线参数是当前接口，下划线形式作为旧调用的别名；它们写入同一字段。
    app.name("DefaultFileAssociation.exe");
    app.set_help_flag("--help", "Show this help text.");
    auto* extension = app.add_option("--ext", data.extension, "File extension, for example .pdf.");
    auto* protocol = app.add_option("--protocol", data.protocol, "Protocol without colon, for example https; reuse an installed ProgID.");
    auto* browser = app.add_option("--browser", data.browser, "RegisteredApplications name; set http, https, .htm and .html together.");
    extension->excludes(protocol)->excludes(browser);
    protocol->excludes(browser);
    app.add_option("--exe-path,--exe_path", data.executable,
                   "Absolute path to the executable that opens the extension.");
    auto* icon_index = app.add_option("--icon-index,--icon_idx", data.icon_index,
                                      "Zero-based icon index in the executable.");
    auto* icon_path = app.add_option("--icon-path,--icon_path", data.icon_path,
                                     "Path to an .ico, .exe, or .dll icon source.");
    icon_index->excludes(icon_path);
    app.add_option("--prog-id,--prog_name", data.prog_id,
                   "Stable ProgID. If omitted it is derived from the executable.");
    app.add_option("--description", data.description,
                   "Description shown by Windows for the association.");
    app.add_flag("--no-hta", data.no_hta, "Disable the legacy HTA fallback.");
    app.add_flag("--dry-run", data.dry_run, "Validate and print without changing the registry.");
    app.add_flag("--check", data.check, "Inspect the current association instead of setting it.");
    app.add_flag("--diagnose", data.diagnose, "Collect read-only registry and Shell diagnostics.");
    app.add_flag("--latest-native-probe", data.latest_native_probe,
                 "Build-specific x64 PDF experiment using an existing ProgID; --check performs read-only validation.");
    app.add_option("--log-file", data.log_file, "Append UTF-8 output and diagnostic snapshots to this file.");
    app.add_option("--experiment", data.experiment,
                   "VM comparison: default (toasts), none, toasts, notify, or toasts-notify.")
        ->check(CLI::IsMember({"default", "none", "toasts", "notify", "toasts-notify"}));
}

bool NormalizeExtension(std::wstring& value) {
    // 保留前导点并转小写：“ .PDF ”得到“.pdf”。拒绝分隔符，防止扩展名变成子键路径。
    // 校验长度时包括前导点；这一步只规范化扩展名，不修改磁盘上的文件名。
    while (!value.empty() && std::iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::iswspace(value.back())) {
        value.pop_back();
    }
    if (value.size() < 2 || value.size() > 255 || value.front() != L'.') {
        return false;
    }
    for (const wchar_t character : value) {
        if (character < 0x20 || std::iswspace(character) ||
            std::wstring_view(L"\\/:*?\"<>|").find(character) != std::wstring_view::npos) {
            return false;
        }
    }
    value = platform::ToLower(std::move(value));
    return true;
}

using association::IsValidProgId;

std::wstring DefaultProgId(const Options& options) {
    // C:\Apps\DocBox\DocBox.exe 的 stem 是 DocBox；ext=.pdf 去掉点后是 pdf。
    // 拼接的结果是 ProgID“DocBox.pdf”，它是注册表标识，不是磁盘路径或 EXE 文件名。
    // 中文等不允许进入当前 ProgID 字符集的字符替换为下划线；不截断长标识以免产生冲突。
    auto sanitize = [](std::wstring value) {
        for (auto& character : value) {
            if (!IsValidProgId(std::wstring_view(&character, 1))) {
                character = L'_';
            }
        }
        const auto first = value.find_first_not_of(L'.');
        return first == std::wstring::npos ? std::wstring{} : value.substr(first);
    };
    auto stem = sanitize(options.executable.stem().wstring());
    auto extension = sanitize(options.extension.substr(1));
    if (stem.empty()) {
        stem = L"DefaultFileAssociation";
    }
    return extension.empty() ? stem : stem + L"." + extension;
}

bool IsExistingRegularFile(const std::filesystem::path& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}

std::wstring Validate(Options& options) {
    // 所有参数先校验，之后才能打开日志、注册程序或修改 UserChoice。
    // 文件/协议的只读模式需显式给出期望 ProgID；浏览器模式则从已安装 Capabilities 解析。
    const int targets = !options.extension.empty() + !options.protocol.empty() + !options.browser.empty();
    if (targets != 1) {
        return L"Specify exactly one of --ext, --protocol or --browser.";
    }
    if (!options.extension.empty() && !NormalizeExtension(options.extension)) {
        return L"--ext must be a valid dot-prefixed extension of at most 255 characters, for example .pdf.";
    }
    if (options.icon_index && *options.icon_index < 0) {
        return L"--icon-index must be zero or greater.";
    }
    if (!options.prog_id.empty() && !IsValidProgId(options.prog_id)) {
        return L"--prog-id must be at most 255 characters using letters, digits, '.', '-' and '_'.";
    }
    if ((options.check && options.diagnose) ||
        ((options.check || options.diagnose) && (options.dry_run ||
         (options.experiment != "default" && options.experiment != "none")))) {
        return L"Read-only modes cannot be combined with experiments, --dry-run, or each other.";
    }
    if (options.latest_native_probe) {
        if (options.extension != L".pdf" || options.prog_id.empty() ||
            !options.executable.empty() || !options.icon_path.empty() || options.icon_index ||
            !options.description.empty() || options.diagnose || options.dry_run ||
            (options.experiment != "default" && options.experiment != "none")) {
            return L"--latest-native-probe requires --ext=.pdf and an existing --prog-id; only --check, --no-hta and --log-file may accompany it.";
        }
        options.use_hta = false;
        return {};
    }
    if (!options.protocol.empty() || !options.browser.empty()) {
        if (!options.executable.empty() || !options.icon_path.empty() || options.icon_index ||
            !options.description.empty()) {
            return L"Protocol/browser modes reuse installed registration; executable, icon and description options are not allowed.";
        }
        if (!options.browser.empty()) {
            if (!options.prog_id.empty()) {
                return L"--browser resolves the browser's ProgIDs; do not supply --prog-id.";
            }
            return {};
        }
        const auto letter = [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
        };
        if (options.protocol.size() > 255 || !letter(options.protocol.front()) ||
            !std::ranges::all_of(options.protocol, [&](wchar_t c) {
                return letter(c) || (c >= L'0' && c <= L'9') || c == L'+' || c == L'-' || c == L'.';
            })) {
            return L"--protocol must be a URI scheme without a colon, for example http or https.";
        }
        options.protocol = platform::ToLower(std::move(options.protocol));
        return options.prog_id.empty() ? L"--prog-id is required with --protocol." : std::wstring{};
    }
    if (options.check || options.diagnose) {
        if (options.prog_id.empty()) {
            return L"--prog-id is required with --check or --diagnose.";
        }
        if (!options.executable.empty() || !options.icon_path.empty() || options.icon_index) {
            return L"--check/--diagnose cannot be combined with executable or icon options.";
        }
        if ((options.experiment != "default" && options.experiment != "none") ||
            options.dry_run || (options.check && options.diagnose)) {
            return L"Read-only modes cannot be combined with experiments, --dry-run, or each other.";
        }
        return {};
    }
    if (!options.executable.is_absolute()) {
        return L"--exe-path is required and must be an absolute path.";
    }
    DWORD binary_type = 0;
    // 既检查绝对路径上的普通文件存在，也检查其 Windows 可执行格式。
    // 本校验不启动目标程序，也不保证目标程序支持所选文档类型。
    if (!IsExistingRegularFile(options.executable) ||
        !::GetBinaryTypeW(options.executable.c_str(), &binary_type)) {
        return L"--exe-path must point to an existing Windows executable.";
    }
    if (binary_type != SCS_32BIT_BINARY && binary_type != SCS_64BIT_BINARY) {
        return L"--exe-path must point to a 32-bit or 64-bit Windows application.";
    }
    if (binary_type == SCS_64BIT_BINARY && !platform::Is64BitWindows()) {
        return L"A 64-bit reader cannot run on 32-bit Windows.";
    }
    if (!options.icon_path.empty() &&
        (!options.icon_path.is_absolute() || !IsExistingRegularFile(options.icon_path))) {
        return L"--icon-path must point to an existing absolute file path.";
    }
    if (options.prog_id.empty()) {
        options.prog_id = DefaultProgId(options);
        // 自动生成的值也必须在任何日志或注册表写入之前完成校验。
        if (!IsValidProgId(options.prog_id)) {
            return L"The generated ProgID exceeds 255 characters; provide a shorter --prog-id.";
        }
    }
    return {};
}

} // namespace

ParseResult Parse(int argc, wchar_t** argv) {
    ParseResult result;
    if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
        result.error = L"Invalid command-line arguments.";
        return result;
    }

    ParserData data;
    CLI::App app("Register or inspect a per-user Windows file association.");
    Configure(app, data);
    std::vector<const wchar_t*> arguments{argv[0]};
    int first_option = 1;
    if (argc > 1 && argv[1] != nullptr) {
        const std::wstring_view action(argv[1]);
        if (action == L"check" || action == L"set-default") {
            data.check = action == L"check";
            first_option = 2;
        }
    }
    for (int index = first_option; index < argc; ++index) {
        if (argv[index] == nullptr) {
            result.error = L"Invalid command-line arguments.";
            return result;
        }
        arguments.push_back(argv[index]);
    }

    try {
        // Windows argv 是 UTF-16；CLI11 处理与内部 UTF-8 参数的转换，避免自行维护编码代码。
        app.parse(static_cast<int>(arguments.size()), arguments.data());
        Options& options = result.options;
        options.check = data.check;
        options.diagnose = data.diagnose;
        options.latest_native_probe = data.latest_native_probe;
        options.experiment = data.experiment;
        options.log_file = CLI::widen(data.log_file);
        options.dry_run = data.dry_run;
        options.use_hta = !data.no_hta;
        options.extension = CLI::widen(data.extension);
        options.protocol = CLI::widen(data.protocol);
        options.browser = CLI::widen(data.browser);
        options.executable = CLI::widen(data.executable);
        options.icon_path = CLI::widen(data.icon_path);
        options.prog_id = CLI::widen(data.prog_id);
        options.description = CLI::widen(data.description);
        if (app.count("--icon-index") != 0) {
            options.icon_index = data.icon_index;
        }
        result.error = Validate(options);
        result.ok = result.error.empty();
    } catch (const CLI::CallForHelp&) {
        result.ok = result.show_help = true;
    } catch (const std::exception& error) {
        result.error = CLI::widen(error.what());
    }
    return result;
}

std::wstring HelpText() {
    ParserData data;
    CLI::App app("Register or inspect a per-user Windows file association.");
    Configure(app, data);
    return CLI::widen(app.help());
}

} // namespace dfa::cli
