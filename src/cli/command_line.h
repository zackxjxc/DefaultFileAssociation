#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace dfa::cli {

struct Options {
    // Parse 成功后文件后缀或协议已规范化；文件设置模式的 prog_id 已生成并校验。
    // --browser 只保留注册名称，各项 ProgID 由后续只读的 Capabilities 解析产生。
    bool check = false;
    bool dry_run = false;
    bool use_hta = true;
    bool diagnose = false;
    bool latest_native_probe = false;
    std::string experiment = "default";
    std::filesystem::path log_file;

    std::wstring extension;
    std::wstring protocol;
    std::wstring browser;
    std::filesystem::path executable;
    std::optional<int> icon_index;
    std::filesystem::path icon_path;
    std::wstring prog_id;
    std::wstring description;
};

struct ParseResult {
    bool ok = false;
    bool show_help = false;
    Options options;
    std::wstring error;
};

// 接收 wmain 提供的 UTF-16 参数，默认执行设置；set-default 是可选动作名，check 等同 --check。
// 参数失败通过 ParseResult 返回，不在解析阶段打开日志、写注册表或启动子进程。
ParseResult Parse(int argc, wchar_t** argv);

// 返回与 --help 相同的说明，避免维护两份参数清单。
std::wstring HelpText();

} // namespace dfa::cli
