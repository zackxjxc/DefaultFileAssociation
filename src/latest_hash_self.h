#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace dfa::latest_hash {

struct Result {
    bool ok = false;
    std::wstring value;
    std::wstring machine_id;
    std::wstring input;
    std::wstring compressed;
    std::wstring error;
};

// Reads HKLM\\SOFTWARE\\Microsoft\\SQMClient\\MachineId and removes
// surrounding braces exactly as the Windows helper does.
[[nodiscard]] std::wstring ReadMachineId();

// Independent HashVersion=1 implementation used by UserChoiceLatest.
// It is a best-effort reimplementation of the 26H2 compressor/table format;
// callers should compare the result with the live record before writing.
[[nodiscard]] Result Generate(
    std::wstring_view machine_id,
    std::wstring_view user_sid,
    std::wstring_view extension,
    std::wstring_view prog_id,
    const SYSTEMTIME& timestamp);

} // namespace dfa::latest_hash
