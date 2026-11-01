#include "latest_hash_self.h"

#include <cassert>
#include <iostream>

int main() {
    SYSTEMTIME timestamp{2026, 11, 0, 1, 0, 0, 0, 0};
    const auto result = dfa::latest_hash::Generate(
        L"925891D5-4AB6-4FCE-8AC4-EF5C74F2E463",
        L"S-1-5-21-2731269935-3373740355-1041665738-1002",
        L".pdf",
        L"docboxtest.pdf",
        timestamp);
    if (!result.ok) {
        std::wcerr << result.error << L"\n";
        return 1;
    }
    // Captured from the 26H2 windows.storage.dll oracle for the same fixed vector.
    if (result.value != L"jK38+cnJHWE=") {
        std::wcerr << L"unexpected latest hash: " << result.value << L" input=" << result.input
                   << L" compressed_size=" << result.compressed.size() << L" units=";
        for (const auto unit : result.compressed) std::wcerr << std::hex << static_cast<unsigned int>(unit) << L",";
        std::wcerr << std::dec << L"\n";
        return 2;
    }
    if (result.compressed.size() < 2 ||
        result.compressed[0] != static_cast<wchar_t>(199) ||
        result.compressed[1] != static_cast<wchar_t>(25)) {
        std::wcerr << L"compressed metadata mismatch\n";
        return 3;
    }
    return 0;
}
