#pragma once

#include "result.h"
#include "registry.h"

#include <filesystem>
#include <optional>
#include <string>

namespace dfa::association {

struct Specification {
    // 示例：extension=.pdf、executable=C:\Apps\SamplePdfReader\SamplePdfReader.exe、prog_id=SamplePdfReader.pdf。
    // executable/icon_path 是磁盘路径；extension/prog_id 是注册表键名称的一部分。
    // 为兼容原接口保留 extension 字段名；协议模式中其值为 http/https，不带点。
    std::wstring extension;
    std::filesystem::path executable;
    std::wstring prog_id;
    std::filesystem::path icon_path;
    std::optional<int> icon_index;
    std::wstring description;
    bool use_hta = true;
    // Win10+ 默认补齐当前扩展名的关联确认记录；none 实验可关闭以作对照。
    bool update_association_toasts = true;
    // 同步通知仍只用于对照实验，正常流程不增加等待。
    bool experimental_notify = false;
    // 协议/浏览器模式复用 HKCR 中已安装程序的命令，不覆盖其 ProgID、图标或 Capabilities。
    bool reuse_registered = false;
};

[[nodiscard]] registry::Result<std::wstring> ReadRegisteredCommand(std::wstring_view prog_id);

// 注册候选应用及图标、命令；单独调用不保证其已成为系统有效默认应用。
[[nodiscard]] Result RegisterCandidate(const Specification& specification);
// 注册后按系统版本设置默认选择，最后用系统接口验证实际结果。
[[nodiscard]] Result SetDefault(const Specification& specification);
// 仅读取并检查，不修改关联记录；用于验证本工具的注册布局。
[[nodiscard]] Result CheckDefault(const Specification& specification);

} // namespace dfa::association
