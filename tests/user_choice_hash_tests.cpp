#include "crypto/user_choice_hash.h"
#include "crypto/win_crypto.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Test failure: %s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

std::span<const std::byte> Bytes(const char* text, size_t length) {
    return {reinterpret_cast<const std::byte*>(text), length};
}

void TestDigestsAndBase64() {
    const auto md5 = dfa::crypto::Md5(Bytes("abc", 3));
    Require(md5.size() == 16, "MD5 length");
    Require(dfa::crypto::Base64Encode(md5) == L"kAFQmDzST7DWlj99KOF/cg==", "MD5 vector");
    Require(dfa::crypto::Base64Encode(Bytes("hello", 5)) == L"aGVsbG8=", "Base64 vector");
}

void TestUserChoiceHash() {
    SYSTEMTIME timestamp{};
    timestamp.wYear = 2024;
    timestamp.wMonth = 1;
    timestamp.wDay = 2;
    timestamp.wHour = 3;
    timestamp.wMinute = 4;
    timestamp.wSecond = 56;
    timestamp.wMilliseconds = 789;

    const std::wstring extension = L".PDF";
    const std::wstring prog_id = L"SamplePdfReader.PDF";
    const std::wstring sid = L"S-1-5-21-123-456-789-1001";

    const dfa::platform::WindowsVersion windows7{6, 1, 7601};
    const auto legacy = dfa::crypto::GenerateUserChoiceHash(
        extension, prog_id, sid, timestamp, windows7);
    Require(legacy.ok, "legacy hash status");
    Require(legacy.value == L"oMWz0vnsGGs=", "legacy hash vector");
    Require(legacy.input == L".pdfs-1-5-21-123-456-789-1001samplepdfreader.pdf", "legacy input");
    const auto windows8 = dfa::crypto::GenerateUserChoiceHash(
        extension, prog_id, sid, timestamp, dfa::platform::WindowsVersion{6, 2, 9200});
    Require(windows8.ok && windows8.value == legacy.value, "Windows 8 legacy hash");
    Require(dfa::crypto::SelectHashVersion({10, 0, 10586}) ==
                dfa::crypto::HashVersion::Windows10Threshold1,
            "Windows 10 1511 boundary");
    Require(dfa::crypto::SelectHashVersion({10, 0, 10587}) ==
                dfa::crypto::HashVersion::Windows10Threshold2OrLater,
            "Post-1511 boundary");
    Require(dfa::crypto::HashVersionName(dfa::crypto::HashVersion::Legacy) == L"Legacy" &&
                dfa::crypto::HashVersionName(dfa::crypto::HashVersion::Windows10Threshold1) ==
                    L"Windows10Threshold1" &&
                dfa::crypto::HashVersionName(dfa::crypto::HashVersion::Windows10Threshold2OrLater) ==
                    L"Windows10Threshold2OrLater",
            "Hash version names");

    const dfa::platform::WindowsVersion windows10_1507{10, 0, 10240};
    const auto threshold1 = dfa::crypto::GenerateUserChoiceHash(
        extension, prog_id, sid, timestamp, windows10_1507);
    Require(threshold1.ok, "Windows 10 Threshold 1 hash status");
    Require(threshold1.value == L"AYGV9SxOjdk=", "Windows 10 Threshold 1 hash vector");
    Require(threshold1.input ==
                L".pdfs-1-5-21-123-456-789-1001samplepdfreader.pdf01da3d28554dd000"
                L"user choice set via windows user experience "
                L"{480368b3-f2e4-45ae-ba6d-852c4f639a40}",
            "Windows 10 Threshold 1 input");

    const dfa::platform::WindowsVersion windows11{10, 0, 22621};
    const auto threshold2 = dfa::crypto::GenerateUserChoiceHash(
        extension, prog_id, sid, timestamp, windows11);
    Require(threshold2.ok, "Windows 10 Threshold 2 hash status");
    Require(threshold2.value == L"/DwAmVptZUY=", "Windows 10 Threshold 2 hash vector");
    Require(threshold2.input.ends_with(
                L"{d18b6dd5-6124-4341-9318-804003bafa0b}"),
            "Windows 10 Threshold 2 input");
    timestamp.wSecond = timestamp.wMilliseconds = 0;
    const auto minute = dfa::crypto::GenerateUserChoiceHash(
        extension, prog_id, sid, timestamp, windows11);
    Require(minute.ok && minute.value == threshold2.value, "Hash truncates seconds and milliseconds");
    timestamp.wSecond = 59;
    timestamp.wMilliseconds = 999;
    const auto last_millisecond = dfa::crypto::GenerateUserChoiceHash(
        extension, prog_id, sid, timestamp, windows11);
    Require(last_millisecond.ok && last_millisecond.value == threshold2.value,
            "Last millisecond stays in the current minute");
    ++timestamp.wMinute;
    timestamp.wSecond = timestamp.wMilliseconds = 0;
    const auto next_minute = dfa::crypto::GenerateUserChoiceHash(
        extension, prog_id, sid, timestamp, windows11);
    Require(next_minute.ok && next_minute.value != threshold2.value,
            "Minute rollover requires a new hash");
    const auto accented = dfa::crypto::GenerateUserChoiceHash(
        L".ÉXT", prog_id, sid, timestamp, windows11);
    const auto lowercase = dfa::crypto::GenerateUserChoiceHash(
        L".éxt", prog_id, sid, timestamp, windows11);
    Require(accented.ok && lowercase.ok && accented.value == lowercase.value &&
                accented.input.starts_with(L".éxt"), "Hash uses Windows Unicode lowercase");
}

} // namespace

int main() {
    TestDigestsAndBase64();
    TestUserChoiceHash();
    return 0;
}
