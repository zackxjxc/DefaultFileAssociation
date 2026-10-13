#pragma once

#include "result.h"

#include <string>
#include <string_view>

namespace dfa::latest_native {

// Calls the analyzed x64 Windows DLL in this process. No child process, HTA,
// registration cleanup or protection changes. Writes are never skipped merely
// because the requested ProgID is already current; this is a write experiment.
[[nodiscard]] Result Probe(std::wstring_view prog_id, bool attempt_set, std::wstring& trace);

} // namespace dfa::latest_native
