#pragma once

#include "association.h"

namespace dfa::msbuild {
enum class Mode { Preflight, Check, Set };
// Framework64 host; new-format PDF only. Never invokes the legacy HTA path.
[[nodiscard]] Result Run(const association::Specification& request, Mode mode, std::wstring& trace);
}
