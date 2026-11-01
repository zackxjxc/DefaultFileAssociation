#pragma once

#include "result.h"

#include <string>
#include <string_view>

namespace dfa::latest_native {

// Validate the current build, HashVersion and existing UserChoiceLatest hash
// without requiring the target ProgID to be registered and without writes.
[[nodiscard]] Result Preflight(std::wstring& trace);

// Uses the analyzed x64 Windows DLL when its fingerprint is known, otherwise
// the independent latest-hash implementation. No child process, HTA,
// registration cleanup or protection changes. Writes are never skipped merely
// because the requested ProgID is already current; this is a write experiment.
[[nodiscard]] Result Probe(std::wstring_view prog_id, bool attempt_set, std::wstring& trace);

} // namespace dfa::latest_native
