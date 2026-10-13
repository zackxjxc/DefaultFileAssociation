#include "user_choice.h"

#include "crypto/user_choice_hash.h"
#include "platform.h"
#include "registry.h"
#include "hta_workspace.h"
#include "association_target.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <wil/resource.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace dfa::user_choice {
namespace {

constexpr std::wstring_view kUserChoiceValueProgId = L"ProgId";
constexpr std::wstring_view kUserChoiceValueHash = L"Hash";

std::wstring UserChoiceKey(std::wstring_view extension) {
    // 返回相对 HKCU 的路径，例如 Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoice。
    // 协议则为 Software\Microsoft\Windows\Shell\Associations\UrlAssociations\https\UserChoice。
    // 后续 ProgId 和 Hash 均写入此键；标识已由命令行校验，不允许带路径分隔符。
    return association::ChoiceKey(extension);
}

bool SameMinute(const SYSTEMTIME& left, const SYSTEMTIME& right) {
    // 比较完整 UTC 日期和分钟，跨小时、跨天、跨月都不能误认为仍处于同一分钟。
    return left.wYear == right.wYear && left.wMonth == right.wMonth && left.wDay == right.wDay &&
           left.wHour == right.wHour && left.wMinute == right.wMinute;
}

crypto::HashResult CurrentHash(const Request& request, const std::wstring& user_sid,
                               SYSTEMTIME& timestamp, const platform::WindowsVersion& version) {
    ::GetSystemTime(&timestamp);
    return crypto::GenerateUserChoiceHash(request.extension, request.prog_id, user_sid, timestamp, version);
}

Result HashFailure(const crypto::HashResult& hash) {
    return {Status::Failed, L"Unable to calculate the UserChoice hash: " + hash.error, 0, false};
}

registry::Result<void> RemoveUserChoiceWriteDeny(const std::wstring& key, const std::wstring& user_sid) {
    // 仅删除本用户所拥有 UserChoice 键中，针对本人且只禁止 SET_VALUE 的显式 ACE。
    // 保留其他权限与所有者；UCPD 仍可能拦截写入，随后再尝试 HTA。
    auto error = [](DWORD status) -> registry::Result<void> {
        return std::unexpected(registry::Error{static_cast<LSTATUS>(status), L"UserChoice write restriction"});
    };
    wil::unique_hkey handle;
    LSTATUS status = ::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0,
                                     READ_CONTROL | WRITE_DAC, handle.put());
    if (status == ERROR_FILE_NOT_FOUND) {
        return {};
    }
    if (status != ERROR_SUCCESS) {
        return error(status);
    }
    PSID raw_sid = nullptr;
    if (!::ConvertStringSidToSidW(user_sid.c_str(), &raw_sid)) {
        return error(::GetLastError());
    }
    wil::unique_hlocal_ptr<void> sid(raw_sid);
    PSID owner = nullptr;
    PACL original = nullptr;
    wil::unique_hlocal_security_descriptor descriptor;
    status = ::GetSecurityInfo(handle.get(), SE_REGISTRY_KEY,
                                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                &owner, nullptr, &original, nullptr, descriptor.put());
    if (status != ERROR_SUCCESS) {
        return error(status);
    }
    if (!owner || !::EqualSid(owner, sid.get())) {
        // 不修改其他用户拥有的键，也不尝试取得所有权；权限异常交给错误与 HTA 回退处理。
        return error(ERROR_ACCESS_DENIED);
    }
    if (!original) {
        return {};
    }
    std::vector<BYTE> buffer(original->AclSize);
    auto* replacement = reinterpret_cast<PACL>(buffer.data());
    if (!::InitializeAcl(replacement, static_cast<DWORD>(buffer.size()), original->AclRevision)) {
        return error(::GetLastError());
    }
    bool removed = false;
    for (DWORD index = 0; index < original->AceCount; ++index) {
        // 按原顺序复制 ACE，仅跳过精确匹配的显式拒绝项，保留其他权限和继承关系。
        void* raw = nullptr;
        if (!::GetAce(original, index, &raw)) {
            return error(::GetLastError());
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE && !(header->AceFlags & INHERITED_ACE)) {
            const auto* denied = static_cast<const ACCESS_DENIED_ACE*>(raw);
            if (denied->Mask == KEY_SET_VALUE &&
                ::EqualSid(const_cast<DWORD*>(&denied->SidStart), sid.get())) {
                removed = true;
                continue;
            }
        }
        if (!::AddAce(replacement, original->AclRevision, MAXDWORD, raw, header->AceSize)) {
            return error(::GetLastError());
        }
    }
    if (!removed) {
        return {};
    }
    SECURITY_DESCRIPTOR updated{};
    if (!::InitializeSecurityDescriptor(&updated, SECURITY_DESCRIPTOR_REVISION) ||
        !::SetSecurityDescriptorDacl(&updated, TRUE, replacement, FALSE)) {
        return error(::GetLastError());
    }
    status = ::RegSetKeySecurity(handle.get(), DACL_SECURITY_INFORMATION, &updated);
    return status == ERROR_SUCCESS ? registry::Result<void>{} : error(status);
}

std::wstring EscapeVbString(std::wstring_view value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const wchar_t character : value) {
        escaped.push_back(character);
        if (character == '"') {
            escaped.push_back('"');
        }
    }
    return escaped;
}

bool CheckUserChoice(const Request& request) {
    // 校验必须使用键的真实最后写入时间，不能用“现在”去验证此前写入的 Hash。
    // 例如从 HKCU\...\FileExts\.pdf\UserChoice 同时读取 ProgId、Hash 和键时间。
    const std::wstring user_choice_key = UserChoiceKey(request.extension);
    auto timestamp = registry::LastWriteTime(HKEY_CURRENT_USER, user_choice_key);
    if (!timestamp) {
        return false;
    }

    auto prog_id = registry::ReadString(HKEY_CURRENT_USER, user_choice_key,
                                        kUserChoiceValueProgId);
    auto hash = registry::ReadString(HKEY_CURRENT_USER, user_choice_key,
                                     kUserChoiceValueHash);
    if (!prog_id || !hash || !platform::EqualsInsensitive(*prog_id, request.prog_id)) {
        return false;
    }

    SYSTEMTIME system_time{};
    if (!::FileTimeToSystemTime(&*timestamp, &system_time)) {
        return false;
    }
    const std::wstring user_sid = platform::GetCurrentUserSid();
    if (user_sid.empty()) {
        return false;
    }

    const crypto::HashResult expected = crypto::GenerateUserChoiceHash(
        request.extension, request.prog_id, user_sid, system_time,
        platform::GetWindowsVersion());
    return expected.ok && expected.value == *hash;
}

bool WriteHtaFile(const std::filesystem::path& path, std::wstring_view content) {
    // 这里只写入已独占创建目录中的 choice.hta，不会写入目标阅读器目录。
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    // MSHTA 通过 UTF-16LE BOM 识别编码，确保中文注册表路径不被错误解码。
    static_assert(sizeof(wchar_t) == 2);
    const wchar_t bom = 0xFEFF;
    output.write(reinterpret_cast<const char*>(&bom), sizeof(bom));
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size() * sizeof(wchar_t)));
    return output.good();
}

bool SetUserChoiceByHta(const Request& request, std::wstring_view hash) {
    // 原阅读器用私有 Base64 字母表隐藏这个固定模板；此处直接保留可审查的明文。
    const std::wstring registry_key = L"HKCU\\" + UserChoiceKey(request.extension);
    // VBScript 需要带根键的完整字符串，例如：
    // HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoice。
    // 模板在其后分别加 \Hash、\ProgId；双引号经 EscapeVbString 转义为 VB 字符串字面量。
    const std::wstring hta = std::format(
        L"<!DOCTYPE html>\n<html><head>\n"
        L"<HTA:APPLICATION APPLICATIONNAME=\"DefaultFileAssociation\" "
        L"BORDER=\"none\" CAPTION=\"no\" SHOWINTASKBAR=\"no\" "
        L"SINGLEINSTANCE=\"yes\" SYSMENU=\"no\" WINDOWSTATE=\"minimize\">\n"
        L"<script language=\"VBScript\">\n"
        L"Sub Window_OnLoad\n"
        L"Set WshShell = CreateObject(\"WScript.Shell\")\n"
        L"On Error Resume Next\n"
        L"WshShell.RegWrite \"{}\\Hash\", \"{}\", \"REG_SZ\"\n"
        L"WshShell.RegWrite \"{}\\ProgId\", \"{}\", \"REG_SZ\"\n"
        L"self.close\nEnd Sub\n</script></head><body></body></html>\n",
        EscapeVbString(registry_key), EscapeVbString(hash),
        EscapeVbString(registry_key), EscapeVbString(request.prog_id));

    std::error_code file_error;
    const auto temp_directory = std::filesystem::temp_directory_path(file_error);
    if (file_error) {
        return false;
    }
    wchar_t temporary_file[MAX_PATH]{};
    // GetTempFileName 创建并保留一个预约文件，例如 <用户临时目录>\dfa1234.tmp。
    // 保留预约文件直到回退结束，减少其他本工具实例获得同一临时名称的机会。
    if (::GetTempFileNameW(temp_directory.c_str(), L"dfa", 0, temporary_file) == 0) {
        return false;
    }
    std::filesystem::path temporary_path(temporary_file);
    auto cleanup_temporary = wil::scope_exit([&] {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    });
    auto directory = temporary_path;
    // 目录由预约文件路径加“.d”得到，例如 C:\Users\Alice\AppData\Local\Temp\dfa1234.tmp.d。
    // HtaWorkspace 原子创建目录，脚本最终为该目录内的 choice.hta；碰撞时直接失败。
    directory += L".d";
    HtaWorkspace workspace(std::move(directory));
    if (!workspace) {
        return false;
    }
    const auto& hta_path = workspace.file();

    bool completed = false;
    if (WriteHtaFile(hta_path, hta)) {
        const auto mshta_path = platform::GetMshtaPath();
        if (mshta_path.empty()) {
            return false;
        }
        // 32 位系统：C:\Windows\System32\mshta.exe；x86/WOW64：C:\Windows\Sysnative\mshta.exe。
        // 启动命令是两个独立、带引号的参数：系统 mshta.exe 路径和临时 choice.hta 路径。
        // 显式指定应用路径，避免在当前目录或 PATH 中寻找同名程序。
        std::wstring command_line = platform::QuoteCommandLineArgument(mshta_path.wstring()) + L" " +
                                     platform::QuoteCommandLineArgument(hta_path.wstring());
        STARTUPINFOW startup_info{sizeof(startup_info)};
        PROCESS_INFORMATION process_info{};
        if (::CreateProcessW(mshta_path.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &startup_info,
                              &process_info)) {
            wil::unique_handle process(process_info.hProcess);
            wil::unique_handle thread(process_info.hThread);
            const DWORD wait_result = ::WaitForSingleObject(process.get(), 10'000);
            // 等待最长十秒；超时只结束本次创建的子进程，不扫描或结束其他 mshta 实例。
            // 即使正常退出也要重新读取 UserChoice；脚本的 On Error Resume Next 不提供成功保证。
            if (wait_result != WAIT_OBJECT_0) {
                ::TerminateProcess(process.get(), ERROR_TIMEOUT);
                ::WaitForSingleObject(process.get(), 2'000);
            } else {
                completed = true;
            }
        }
    }
    return completed;
}

Result Apply(const Request& request) {
    if (CheckUserChoice(request)) {
        return Result{Status::AlreadyDefault,
                      L"The association is already the current user's default.", 0, false};
    }

    const std::wstring user_sid = platform::GetCurrentUserSid();
    if (user_sid.empty()) {
        return Result{Status::Failed, L"Unable to determine the current user's SID.",
                      ::GetLastError(), false};
    }
    const auto version = platform::GetWindowsVersion();
    // 先确认算法可用，再触碰原 UserChoice；正式写入时仍重新取当前时间。
    SYSTEMTIME probe_time{};
    const auto probe = CurrentHash(request, user_sid, probe_time, version);
    if (!probe.ok) {
        return HashFailure(probe);
    }

    const std::wstring user_choice_key = UserChoiceKey(request.extension);
    std::optional<Result> direct_error;
    auto remember_direct_error = [&](const auto& operation, std::wstring_view context) {
        if (!operation && !direct_error) {
            direct_error = Result{
                Status::Failed,
                std::wstring(context) + L": " + registry::DescribeError(operation.error()),
                static_cast<unsigned long>(operation.error().status), false};
        }
    };

    const auto writable = RemoveUserChoiceWriteDeny(user_choice_key, user_sid);
    remember_direct_error(writable, L"UserChoice ACL");
    // 记录直接写入的首个错误，但不中断后面的兼容路径；最终以读回校验为准。
    const auto deleted = registry::DeleteTree(HKEY_CURRENT_USER, user_choice_key);
    // 删除后仍尝试写入：某些系统允许修改现有键，但不允许删除；最终以读回校验为准。
    remember_direct_error(deleted, L"UserChoice cleanup");
    // 清理完成后才取时间。若计算与写入跨分钟，立即重算并有限重试，不固定等待。
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        SYSTEMTIME timestamp{};
        const auto hash = CurrentHash(request, user_sid, timestamp, version);
        if (!hash.ok) {
            return HashFailure(hash);
        }
        const auto prog_id = registry::SetString(HKEY_CURRENT_USER, user_choice_key,
                                                 kUserChoiceValueProgId, request.prog_id);
        remember_direct_error(prog_id, L"UserChoice ProgId");
        const auto hash_value = registry::SetString(HKEY_CURRENT_USER, user_choice_key,
                                                    kUserChoiceValueHash, hash.value);
        remember_direct_error(hash_value, L"UserChoice hash");
        if (CheckUserChoice(request)) {
            return Result{Status::Success, L"The current user's default association was set.", 0, false};
        }
        SYSTEMTIME after{};
        ::GetSystemTime(&after);
        if (!version.IsWindows10OrGreater() || SameMinute(timestamp, after)) {
            break;
        }
    }

    bool hta_attempted = false;
    // mshta 退出并不代表写入成功，必须重新验证 ProgID、哈希与键的写入时间。
    if (request.use_hta && version.IsWindows10OrGreater()) {
        hta_attempted = true;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            // HTA 启动可能延迟，不能沿用直接写入阶段的旧哈希。
            SYSTEMTIME timestamp{};
            const auto hash = CurrentHash(request, user_sid, timestamp, version);
            if (!hash.ok) {
                Result result = HashFailure(hash);
                result.hta_attempted = true;
                return result;
            }
            const bool completed = SetUserChoiceByHta(request, hash.value);
            if (CheckUserChoice(request)) {
                return Result{Status::Success,
                              L"The current user's default association was set through the HTA compatibility path.",
                              0, true};
            }
            SYSTEMTIME after{};
            ::GetSystemTime(&after);
            if (!completed || SameMinute(timestamp, after)) {
                break;
            }
        }
    }
    if (direct_error) {
        direct_error->hta_attempted = hta_attempted;
        return *direct_error;
    }
    return Result{Status::UserChoiceRejected,
                  L"Windows rejected the UserChoice update. Use the Windows Default Apps settings page.",
                  0, hta_attempted};
}

} // namespace

bool Check(const Request& request) {
    return CheckUserChoice(request);
}

Result SetDefault(const Request& request) {
    return Apply(request);
}

} // namespace dfa::user_choice
