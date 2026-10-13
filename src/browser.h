#pragma once

#include "association.h"
#include <vector>

namespace dfa::browser {

// name 是 RegisteredApplications 的值名称（如 Google Chrome），不是 EXE 文件名。
// 解析并预检全部四项后才返回计划；缺失任何关联都不会进入写入阶段。
[[nodiscard]] registry::Result<std::vector<association::Specification>> Resolve(
    std::wstring_view name, const association::Specification& defaults);

} // namespace dfa::browser
