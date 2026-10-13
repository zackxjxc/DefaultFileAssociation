#include "user_choice_hash.h"

#include "win_crypto.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

namespace dfa::crypto {
namespace {

// The UserChoice algorithm was reverse-engineered and documented by the
// SFTA/PS-SFTA projects and is also implemented by Mozilla Firefox. This
// independent implementation preserves the behavior used by EnnoiaReader's
// SetDefaultApp.cpp; no Mozilla source is copied here. See:
// https://github.com/DanysysTeam/SFTA
// https://github.com/DanysysTeam/PS-SFTA
// https://searchfox.org/mozilla-central/source/browser/components/shell/WindowsUserChoice.cpp
constexpr std::wstring_view kWindowsUserExperience =
    L"User Choice set via Windows User Experience ";
constexpr std::wstring_view kThreshold1Guid = L"{480368b3-f2e4-45ae-ba6d-852c4f639a40}";
constexpr std::wstring_view kThreshold2Guid = L"{d18b6dd5-6124-4341-9318-804003bafa0b}";
// 两个 GUID 是原阅读器使用的系统兼容常量，不是每次运行随机生成的应用 ID。

[[nodiscard]] std::uint32_t WordSwap(std::uint32_t value) noexcept {
    return (value >> 16) | (value << 16);
}

[[nodiscard]] std::uint32_t ReadDword(const std::byte* bytes) noexcept {
    // Windows 目标使用小端序；memcpy 避免将未对齐字节直接解引用为 uint32_t。
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

[[nodiscard]] std::wstring Scramble(std::span<const std::byte> input) {
    // 每个完整块含两个 32 位字；全部输入参与 MD5，混合阶段只处理完整的八字节块。
    // 所有算术保持 uint32_t 的模 2^32 溢出语义，不能改成有符号整数或随意扩成 64 位。
    constexpr size_t kDwordsPerBlock = 2;
    constexpr size_t kBlockSize = kDwordsPerBlock * sizeof(std::uint32_t);
    const size_t block_count = input.size() / kBlockSize;
    if (block_count == 0) {
        return {};
    }

    const auto digest = Md5(input);
    const std::uint32_t md5_0 = ReadDword(digest.data());
    const std::uint32_t md5_1 = ReadDword(digest.data() + sizeof(std::uint32_t));

    constexpr std::uint32_t kC0[kDwordsPerBlock][5] = {
        {0x00000000u, 0xCF98B111u, 0x87085B9Fu, 0x12CEB96Du, 0x257E1D83u},
        {0x00000000u, 0xA27416F5u, 0xD38396FFu, 0x7C932B89u, 0xBFA49F69u},
    };
    constexpr std::uint32_t kC1[kDwordsPerBlock][5] = {
        {0x00000000u, 0xEF0569FBu, 0x689B6B9Fu, 0x79F8A395u, 0xC3EFEA97u},
        {0x00000000u, 0xC31713DBu, 0xDDCD1F0Fu, 0x59C3AF2Du, 0x35BD1EC9u},
    };

    std::uint32_t c0[kDwordsPerBlock][5];
    std::uint32_t c1[kDwordsPerBlock][5];
    std::memcpy(c0, kC0, sizeof(c0));
    std::memcpy(c1, kC1, sizeof(c1));
    c0[0][0] = md5_0 | 1u;
    c0[1][0] = md5_1 | 1u;
    c1[0][0] = md5_0 | 1u;
    c1[1][0] = md5_1 | 1u;

    std::uint32_t h0 = 0;
    std::uint32_t h1 = 0;
    std::uint32_t h0_acc = 0;
    std::uint32_t h1_acc = 0;

    for (size_t block = 0; block < block_count; ++block) {
        for (size_t word = 0; word < kDwordsPerBlock; ++word) {
            const std::byte* word_bytes = input.data() +
                (block * kDwordsPerBlock + word) * sizeof(std::uint32_t);
            const std::uint32_t value = ReadDword(word_bytes);

            h0 += value;
            h0 *= c0[word][0];
            h0 = WordSwap(h0) * c0[word][1];
            h0 = WordSwap(h0) * c0[word][2];
            h0 = WordSwap(h0) * c0[word][3];
            h0 = WordSwap(h0) * c0[word][4];
            h0_acc += h0;

            h1 += value;
            h1 = WordSwap(h1) * c1[word][1] + h1 * c1[word][0];
            h1 = (h1 >> 16) * c1[word][2] + h1 * c1[word][3];
            h1 = WordSwap(h1) * c1[word][4] + h1;
            h1_acc += h1;
        }
    }

    const std::array<std::byte, sizeof(std::uint32_t) * 2> result = {
        // 两条混合链异或后按小端序输出八字节，随后用标准 Base64 得到注册表 Hash。
        std::byte{static_cast<unsigned char>((h0 ^ h1) & 0xFF)},
        std::byte{static_cast<unsigned char>(((h0 ^ h1) >> 8) & 0xFF)},
        std::byte{static_cast<unsigned char>(((h0 ^ h1) >> 16) & 0xFF)},
        std::byte{static_cast<unsigned char>(((h0 ^ h1) >> 24) & 0xFF)},
        std::byte{static_cast<unsigned char>((h0_acc ^ h1_acc) & 0xFF)},
        std::byte{static_cast<unsigned char>(((h0_acc ^ h1_acc) >> 8) & 0xFF)},
        std::byte{static_cast<unsigned char>(((h0_acc ^ h1_acc) >> 16) & 0xFF)},
        std::byte{static_cast<unsigned char>(((h0_acc ^ h1_acc) >> 24) & 0xFF)},
    };
    return Base64Encode(result);
}

[[nodiscard]] std::wstring MakeInput(
    const std::wstring& extension,
    const std::wstring& prog_id,
    const std::wstring& user_sid,
    const SYSTEMTIME& timestamp,
    HashVersion version) {
    SYSTEMTIME truncated = timestamp;
    // Win10+ 哈希包含 UTC FILETIME，但必须把时间截断到分钟。
    truncated.wSecond = 0;
    truncated.wMilliseconds = 0;

    FILETIME file_time{};
    if (!::SystemTimeToFileTime(&truncated, &file_time)) {
        return {};
    }

    std::wstring input = extension + user_sid + prog_id;
    // 无分隔符拼接，旧版示例：.pdfs-1-5-21-123-456-789-1001docbox.pdf。
    // Win10+ 再追加十六位 FILETIME 十六进制文本、固定体验说明和对应 GUID，整体转小写。
    if (version == HashVersion::Windows10Threshold1 ||
        version == HashVersion::Windows10Threshold2OrLater) {
        wchar_t file_time_text[17]{};
        swprintf_s(file_time_text, 17, L"%08lx%08lx", file_time.dwHighDateTime, file_time.dwLowDateTime);
        input += file_time_text;
        input += kWindowsUserExperience;
        input += version == HashVersion::Windows10Threshold1 ? kThreshold1Guid : kThreshold2Guid;
    }
    return platform::ToLower(std::move(input));
}

} // namespace

HashVersion SelectHashVersion(const platform::WindowsVersion& version) noexcept {
    // Win8/8.1 使用 Legacy；Win10 1507/1511 使用 Threshold1，后续 Win10/11 使用另一常量。
    // 这里保留原阅读器的分界，实际系统是否接受仍由写后 Hash 和 Shell 校验共同决定。
    if (!version.IsWindows10OrGreater()) {
        return HashVersion::Legacy;
    }
    return version.build <= 10586 ? HashVersion::Windows10Threshold1
                                  : HashVersion::Windows10Threshold2OrLater;
}

HashResult GenerateUserChoiceHash(
    const std::wstring& extension,
    const std::wstring& prog_id,
    const std::wstring& user_sid,
    const SYSTEMTIME& timestamp,
    const platform::WindowsVersion& version) {
    return GenerateUserChoiceHash(extension, prog_id, user_sid, timestamp,
                                   SelectHashVersion(version));
}

HashResult GenerateUserChoiceHash(
    const std::wstring& extension,
    const std::wstring& prog_id,
    const std::wstring& user_sid,
    const SYSTEMTIME& timestamp,
    HashVersion version) {
    HashResult result;
    result.input = MakeInput(extension, prog_id, user_sid, timestamp, version);
    if (result.input.empty()) {
        result.error = L"SystemTimeToFileTime failed or the hash input is empty.";
        return result;
    }

    try {
        std::wstring hash_input = result.input;
        // 输入为小写 UTF-16LE，末尾 NUL 也参与计算，不能按普通字符串长度省略。
        hash_input.push_back(L'\0');
        const auto bytes = std::as_bytes(std::span<const wchar_t>(hash_input.data(), hash_input.size()));
        result.value = Scramble(bytes);
        result.ok = !result.value.empty();
        if (!result.ok) {
            result.error = L"The UserChoice hash input is shorter than one 8-byte block.";
        }
    } catch (...) {
        result.ok = false;
        result.value.clear();
        result.error = L"The operating system crypto API could not calculate the UserChoice hash.";
    }
    return result;
}

} // namespace dfa::crypto
