#pragma once

#include "association.h"

#include <string>

namespace dfa::diagnostics {

// 只读取当前关联状态；不会启动阅读器、显示打开方式界面或写注册表。
[[nodiscard]] std::wstring Snapshot(const association::Specification& specification, bool query_shell = true);

} // namespace dfa::diagnostics
