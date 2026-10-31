#include "association.h"

#include "platform.h"
#include "registry.h"
#include "user_choice.h"
#include "association_target.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wil/com.h>

#include <format>
#include <cwchar>
#include <string_view>
#include <set>
#include <chrono>
#include <thread>

namespace dfa::association {
namespace {

// 以下都是相对于 HKCU 的键路径，实际注册位置分别为：
// HKCU\Software\Classes（当前用户的文件类型定义）和
// HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts（Explorer 的扩展名记录）。
constexpr std::wstring_view kClasses = L"Software\\Classes\\";
constexpr std::wstring_view kFileExts =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\";

std::wstring FormatIcon(const Specification& specification) {
    // 结果示例："C:\Apps\SamplePdfReader\SamplePdfReader.exe",1；独立 ICO 则为 "C:\Icons\pdf.ico"。
    // 这是 DefaultIcon 的字符串值，不是一个把“,1”包含在文件名中的磁盘路径。
    const auto& icon = specification.icon_path.empty()
        ? specification.executable : specification.icon_path;
    std::wstring value = platform::QuoteCommandLineArgument(icon.wstring());
    if (specification.icon_index) {
        value += L"," + std::to_wstring(*specification.icon_index);
    }
    return value;
}

std::wstring OpenCommand(const Specification& specification) {
    // 结果示例："C:\Apps\SamplePdfReader\SamplePdfReader.exe" "%1" %*。
    // %1 由 Shell 替换为待打开文件，%* 传递附加参数；路径加引号以保留空格。
    return std::format(L"{} \"%1\" %*",
                       platform::QuoteCommandLineArgument(specification.executable.wstring()));
}

Result Failure(const registry::Error& error, std::wstring_view context) {
    return {Status::Failed, std::wstring(context) + L": " + registry::DescribeError(error),
            static_cast<unsigned long>(error.status), false};
}

void NotifyShell(bool synchronous = false) {
    // 通知系统刷新关联缓存和图标；通知返回不能证明 UserChoice 已被系统接受。
    ::SHChangeNotify(SHCNE_ASSOCCHANGED,
                    SHCNF_IDLIST | (synchronous ? SHCNF_FLUSH : SHCNF_FLUSHNOWAIT), nullptr, nullptr);
    if (synchronous) {
        // 实验：让系统处理新应用注册通知，再写入 UserChoice。
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
}

registry::Result<void> WriteAssociationToasts(const Specification& specification) {
    // 目标应用必须在集合中；set 同时去掉来自不同 OpenWith 索引的重复候选。
    std::set<std::wstring> candidates{specification.prog_id};
    auto collect = [&](const registry::Result<std::vector<std::wstring>>& names,
                       std::wstring_view prefix = L"") -> registry::Result<void> {
        if (!names) {
            if (names.error().status == ERROR_FILE_NOT_FOUND ||
                names.error().status == ERROR_PATH_NOT_FOUND) {
                return {};
            }
            return std::unexpected(names.error());
        }
        for (const auto& name : *names) {
            if (!name.empty()) {
                candidates.insert(std::wstring(prefix) + name);
            }
        }
        return {};
    };
    // 首次打开时可能仍有候选应用确认提示；只补齐当前扩展名的确认记录。
    // ext=.pdf 时，下面依次读取三个索引：
    // HKCR\.pdf\OpenWithProgids：值名称就是候选 ProgID。
    // HKCR\.pdf\OpenWithList：子键如 samplepdfreader.exe，被补成 Applications\samplepdfreader.exe。
    // HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\OpenWithProgids。
    // HKCR 是合并后的 Classes 视图，此处仅用于读取；写入仍只发生在 HKCU。
    for (auto result : {
             collect(registry::ValueNames(HKEY_CLASSES_ROOT,
                 specification.extension + L"\\OpenWithProgids")),
             collect(registry::SubkeyNames(HKEY_CLASSES_ROOT,
                 specification.extension + L"\\OpenWithList"), L"Applications\\"),
             collect(registry::ValueNames(HKEY_CURRENT_USER,
                 ChoiceParent(specification.extension) + L"\\OpenWithProgids"))}) {
        if (!result) {
            return result;
        }
    }
    // 同时确认原默认候选，协议通常没有文件类型的 OpenWith 索引。
    auto previous = registry::ReadString(HKEY_CURRENT_USER, ChoiceKey(specification.extension), L"ProgId");
    if (previous && !previous->empty()) {
        candidates.insert(*previous);
    }
    for (const auto& candidate : candidates) {
        // 完整键：HKCU\Software\Microsoft\Windows\CurrentVersion\ApplicationAssociationToasts。
        // 值名称按 <ProgID>_<带点扩展名> 拼接，例如 SamplePdfReader.pdf_.pdf，类型 DWORD，数据 0。
        // 这是兼容性确认记录；1809 首次打开已实测，其余版本仍以实际验证为准。
        auto written = registry::SetDword(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts",
            candidate + L"_" + specification.extension, 0);
        if (!written) {
            return written;
        }
    }
    return {};
}

Result CheckShellDefault(const Specification& specification, std::wstring_view expected_command) {
    // 注册表写入成功不等于默认关联生效；还需查询 Shell 实际选择的 ProgID 和命令。
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // 若调用线程已有另一种 COM 单元，可以继续查询，但本次不能替它反初始化。
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return {Status::Failed, L"Unable to initialize the Shell association query.", 0, false};
    }
    wil::unique_couninitialize_call uninitialize(SUCCEEDED(initialized));
    wil::com_ptr<IApplicationAssociationRegistration> registration;
    HRESULT result = S_OK;
    try {
        registration = wil::CoCreateInstance<IApplicationAssociationRegistration>(
            __uuidof(ApplicationAssociationRegistration));
    } catch (const wil::ResultException& error) {
        result = error.GetErrorCode();
    }
    wil::unique_cotaskmem_string current;
    if (SUCCEEDED(result)) {
        // AL_EFFECTIVE 查询系统实际生效的默认应用，而不只判断我们写入的键是否存在。
        result = registration->QueryCurrentDefault(specification.extension.c_str(),
            IsProtocol(specification.extension) ? AT_URLPROTOCOL : AT_FILEEXTENSION, AL_EFFECTIVE, current.put());
    }
    if (FAILED(result) || !current) {
        return {Status::Failed, L"Unable to query the effective Windows default ProgID.", 0, false};
    }
    if (!platform::EqualsInsensitive(current.get(), specification.prog_id)) {
        return {Status::UserChoiceRejected,
                L"Windows still selects ProgID " + std::wstring(current.get()) +
                    L" instead of " + specification.prog_id + L".", 0, false};
    }
    DWORD length = 0;
    // 两次查询：先取字符数，再分配缓冲区读取实际 open 命令。
    // ProgID 正确但命令仍指向旧程序时，也必须判为设置未生效。
    const bool modern_protocol = IsProtocol(specification.extension) && platform::GetWindowsVersion().IsWindows8OrGreater();
    const auto flags = modern_protocol ? static_cast<ASSOCF>(kProtocolQueryFlag) : ASSOCF_NONE;
    // Win7 不支持协议查询标志：先由系统确认有效 ProgID，再按该 ProgID 查询命令。
    const auto* identifier = IsProtocol(specification.extension) && !modern_protocol
        ? current.get() : specification.extension.c_str();
    result = ::AssocQueryStringW(flags, ASSOCSTR_COMMAND,
                                  identifier, L"open", nullptr, &length);
    if (FAILED(result) || length == 0) {
        return {Status::Failed, L"Unable to query the effective Shell open command.", 0, false};
    }
    std::wstring command(length, L'\0');
    result = ::AssocQueryStringW(flags, ASSOCSTR_COMMAND,
                                  identifier, L"open", command.data(), &length);
    if (FAILED(result)) {
        return {Status::Failed, L"Unable to read the effective Shell open command.", 0, false};
    }
    command.resize(std::wcslen(command.c_str()));
    if (!platform::EqualsInsensitive(command, expected_command)) {
        return {Status::UserChoiceRejected,
                L"Windows resolves a different open command: " + command, 0, false};
    }
    return {Status::AlreadyDefault,
            L"Verified Windows default: " + specification.prog_id + L"; open command: " + command,
            0, false};
}

} // namespace

registry::Result<std::wstring> ReadRegisteredCommand(std::wstring_view prog_id) {
    if (!IsValidProgId(prog_id)) {
        return std::unexpected(registry::Error{ERROR_INVALID_PARAMETER, L"Invalid registered ProgID"});
    }
    // HKCR 合并用户和整机定义；读取如 HKCR\FirefoxURL-<安装标识>\shell\open\command。
    // 保留 -osint、-url、--single-argument 等浏览器原有参数，不自行拼接替代命令。
    auto command = registry::ReadString(HKEY_CLASSES_ROOT,
        std::wstring(prog_id) + L"\\shell\\open\\command", L"");
    if (!command) {
        auto error = command.error();
        error.operation = L"Read installed open command for " + std::wstring(prog_id);
        return std::unexpected(error);
    }
    if (command && (command->empty() ||
        (command->find(L"%1") == std::wstring::npos && command->find(L"%L") == std::wstring::npos &&
         command->find(L"%l") == std::wstring::npos))) {
        return std::unexpected(registry::Error{ERROR_INVALID_DATA, L"Registered open command lacks a target placeholder"});
    }
    return command;
}

Result RegisterCandidate(const Specification& specification) {
    if (IsProtocol(specification.extension) && !specification.reuse_registered) {
        return {Status::Failed, L"Protocol mode requires an installed ProgID.", ERROR_INVALID_PARAMETER, false};
    }
    if (specification.reuse_registered) {
        auto command = ReadRegisteredCommand(specification.prog_id);
        if (!command) {
            return Failure(command.error(), L"Registered application");
        }
        if (!IsProtocol(specification.extension)) {
            auto candidate = registry::SetEmptyValue(HKEY_CURRENT_USER,
                std::wstring(kClasses) + specification.extension + L"\\OpenWithProgids", specification.prog_id);
            if (!candidate) {
                return Failure(candidate.error(), L"OpenWithProgids");
            }
        }
        if (specification.update_association_toasts && platform::GetWindowsVersion().IsWindows10OrGreater()) {
            auto written = WriteAssociationToasts(specification);
            if (!written) {
                return Failure(written.error(), L"Association confirmation records");
            }
        }
        NotifyShell(specification.experimental_notify);
        return {Status::RegisteredOnly, L"Reusing the installed application's registration.", 0, false};
    }
    // 以 ext=.pdf、prog_id=SamplePdfReader.pdf 为例，四个完整注册表键为：
    // prog_key：HKCU\Software\Classes\SamplePdfReader.pdf。
    // extension_key：HKCU\Software\Classes\.pdf。
    // file_ext_key：HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf。
    // capabilities：HKCU\Software\DefaultFileAssociation\SamplePdfReader.pdf\Capabilities。
    const std::wstring prog_key = std::wstring(kClasses) + specification.prog_id;
    const std::wstring extension_key = std::wstring(kClasses) + specification.extension;
    const std::wstring file_ext_key = std::wstring(kFileExts) + specification.extension;
    const std::wstring capabilities = L"Software\\DefaultFileAssociation\\" +
                                       specification.prog_id + L"\\Capabilities";
    struct StringEntry {
        std::wstring key;
        std::wstring name;
        std::wstring value;
    };
    // 只注册通用的 open 动作，不把原阅读器的打印参数强加给其他程序。
    // prog_key 下的 DefaultIcon、Application、shell\open\command 分别描述图标、应用与启动命令。
    // 例如命令最终位于 HKCU\Software\Classes\SamplePdfReader.pdf\shell\open\command 的默认值。
    // capabilities\FileAssociations 的“.pdf”值指向 SamplePdfReader.pdf；RegisteredApplications 中的
    // “SamplePdfReader.pdf”值保存相对路径 Software\DefaultFileAssociation\SamplePdfReader.pdf\Capabilities。
    // FileExts\.pdf 的 Progid 是旧式辅助记录，不能代替 Win8+ 的 UserChoice。
    const StringEntry entries[] = {
        {prog_key, L"", specification.description},
        {prog_key + L"\\DefaultIcon", L"", FormatIcon(specification)},
        {prog_key + L"\\Application", L"ApplicationName", specification.description},
        {prog_key + L"\\Application", L"ApplicationIcon", FormatIcon(specification)},
        {prog_key + L"\\shell", L"", L"open"},
        {prog_key + L"\\shell\\open\\command", L"", OpenCommand(specification)},
        {extension_key, L"", specification.prog_id},
        // Capabilities 的应用名须与 RegisteredApplications 的索引名称一致。
        {capabilities, L"ApplicationName", specification.prog_id},
        {capabilities, L"ApplicationDescription", specification.description},
        {capabilities + L"\\FileAssociations", specification.extension, specification.prog_id},
        {L"Software\\RegisteredApplications", specification.prog_id, capabilities},
        {file_ext_key, L"Progid", specification.prog_id},
    };
    for (const auto& entry : entries) {
        auto result = registry::SetString(HKEY_CURRENT_USER, entry.key, entry.name, entry.value);
        if (!result) {
            return Failure(result.error(), entry.key + L" [" + entry.name + L"]");
        }
    }
    // 实际键 HKCU\Software\Classes\.pdf\OpenWithProgids，值名称 SamplePdfReader.pdf。
    // REG_NONE 空数据把程序列为候选，不表示它已经成为默认程序。
    auto open_with = registry::SetEmptyValue(HKEY_CURRENT_USER,
                                             extension_key + L"\\OpenWithProgids",
                                             specification.prog_id);
    if (!open_with) {
        return Failure(open_with.error(), L"OpenWithProgids");
    }
    // 删除 FileExts\.pdf 下可能残留的 Application 值，避免旧 EXE 记录干扰 ProgID 路径。
    auto removed = registry::DeleteValue(HKEY_CURRENT_USER, file_ext_key, L"Application");
    if (!removed) {
        return Failure(removed.error(), L"Explorer Application");
    }
    if (specification.update_association_toasts && platform::GetWindowsVersion().IsWindows10OrGreater()) {
        auto written = WriteAssociationToasts(specification);
        if (!written) {
            return Failure(written.error(), L"Association confirmation records");
        }
    }
    // UserChoice 单独处理，使直接写入失败时仍有机会尝试 Win10+ 的 HTA 回退。
    NotifyShell(specification.experimental_notify);
    return {Status::RegisteredOnly, L"File association registered for the current user.", 0, false};
}

Result CheckDefault(const Specification& specification) {
    if (specification.reuse_registered) {
        auto command = ReadRegisteredCommand(specification.prog_id);
        if (!command) {
            return Failure(command.error(), L"Registered application");
        }
        if (platform::GetWindowsVersion().IsWindows8OrGreater() &&
            !user_choice::Check({specification.extension, specification.prog_id, specification.use_hta})) {
            return {Status::UserChoiceRejected, L"The requested UserChoice is missing or invalid.", 0, false};
        }
        return CheckShellDefault(specification, *command);
    }
    // 本检查验证本工具的注册布局：Classes\.pdf、FileExts\.pdf、ProgID 的 open 命令，
    // 然后验证 UserChoice 哈希和系统查询；不是任意第三方安装器注册布局的通用检查器。
    const std::wstring prog_key = std::wstring(kClasses) + specification.prog_id;
    const auto classes = registry::ReadString(HKEY_CURRENT_USER,
                                               std::wstring(kClasses) + specification.extension, L"");
    const auto explorer = registry::ReadString(HKEY_CURRENT_USER,
                                                std::wstring(kFileExts) + specification.extension, L"Progid");
    if (!classes || !explorer ||
        !platform::EqualsInsensitive(*classes, specification.prog_id) ||
        !platform::EqualsInsensitive(*explorer, specification.prog_id)) {
        return {Status::Failed, L"The current user association does not match the requested ProgID.",
                explorer ? 0u : static_cast<unsigned long>(explorer.error().status), false};
    }
    const auto shell = registry::ReadString(HKEY_CURRENT_USER, prog_key + L"\\shell", L"");
    if (!shell || !platform::EqualsInsensitive(*shell, L"open")) {
        return {Status::Failed, L"The ProgID does not select the open shell verb.",
                shell ? 0u : static_cast<unsigned long>(shell.error().status), false};
    }
    const auto command = registry::ReadString(HKEY_CURRENT_USER, prog_key + L"\\shell\\open\\command", L"");
    if (!command || command->find(L"%1") == std::wstring::npos) {
        return {Status::Failed, L"The registered open command is missing %1.",
                command ? 0u : static_cast<unsigned long>(command.error().status), false};
    }
    if (!specification.executable.empty() &&
        !platform::EqualsInsensitive(*command, OpenCommand(specification))) {
        return {Status::Failed, L"The registered open command targets another executable.", 0, false};
    }
    if (platform::GetWindowsVersion().IsWindows8OrGreater() &&
        !user_choice::Check({specification.extension, specification.prog_id, specification.use_hta})) {
        return {Status::Failed, L"The registry entry exists, but its UserChoice hash does not match.", 0, false};
    }
    return CheckShellDefault(specification, *command);
}

Result SetDefault(const Specification& specification) {
    // 即使已经是默认程序，也更新图标、描述和打开命令，再检查是否需要改 UserChoice。
    const Result registered = RegisterCandidate(specification);
    if (registered.status == Status::Failed) {
        return registered;
    }
    const Result current = CheckDefault(specification);
    if (current.status == Status::AlreadyDefault) {
        return current;
    }
    // Win7 不写哈希；Win8/8.1 使用旧算法，Win10/11 使用版本化哈希和可选 HTA。
    if (!platform::GetWindowsVersion().IsWindows8OrGreater()) {
        // Win7 仍可能有旧 UserChoice 覆盖 Classes；原阅读器会先清除它。
        // ext=.pdf 时删除的是 HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoice。
        auto removed = registry::DeleteTree(HKEY_CURRENT_USER,
            ChoiceKey(specification.extension));
        if (!removed) {
            return Failure(removed.error(), L"Legacy UserChoice cleanup");
        }
        if (specification.reuse_registered) {
            // Win7 的 UserChoice 没有 Hash，文件和协议都只写目标 ProgId，再查系统有效结果。
            auto chosen = registry::SetString(HKEY_CURRENT_USER, ChoiceKey(specification.extension),
                                               L"ProgId", specification.prog_id);
            if (!chosen) {
                return Failure(chosen.error(), L"Legacy UserChoice");
            }
        }
        NotifyShell();
        return CheckDefault(specification);
    }
    Result result = user_choice::SetDefault(
        {specification.extension, specification.prog_id, specification.use_hta});
    NotifyShell();
    if (result.status == Status::Success || result.status == Status::AlreadyDefault) {
        Result checked = CheckDefault(specification);
        checked.hta_attempted = result.hta_attempted;
        if (checked.status == Status::AlreadyDefault) {
            checked.status = Status::Success;
        }
        return checked;
    }
    return result;
}

} // namespace dfa::association
