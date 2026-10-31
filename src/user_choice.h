#pragma once

#include "result.h"

#include <string>

namespace dfa::user_choice {

struct Request {
    // 例如 .pdf + SamplePdfReader.pdf；SID 和系统版本由当前进程获取，调用方不能指定另一个用户。
    std::wstring extension;
    std::wstring prog_id;
    bool use_hta = true;
};

// 检查 ProgId、Hash 和键时间是否一致；这里不查询 Shell，外层 association 还会做有效默认校验。
[[nodiscard]] bool Check(const Request& request);
// 直接设置失败时可尝试 Win10+ 的 HTA；所有路径均运行在当前普通用户上下文。
[[nodiscard]] Result SetDefault(const Request& request);

} // namespace dfa::user_choice
