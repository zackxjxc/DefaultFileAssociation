#include "win_crypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <limits>
#include <stdexcept>
#include <vector>

namespace dfa::crypto {

std::array<std::byte, 16> Md5(std::span<const std::byte> data) {
    // CNG 计算 MD5，只用于兼容 UserChoice；句柄由 WIL 释放，失败以异常交给上层转换为结果。
    if (data.size() > std::numeric_limits<ULONG>::max()) {
        throw std::length_error("UserChoice hash input is too large");
    }
    wil::unique_bcrypt_algorithm algorithm;
    THROW_IF_NTSTATUS_FAILED(::BCryptOpenAlgorithmProvider(
        algorithm.put(), BCRYPT_MD5_ALGORITHM, nullptr, 0));

    ULONG object_size = 0;
    ULONG property_size = 0;
    THROW_IF_NTSTATUS_FAILED(::BCryptGetProperty(
        algorithm.get(), BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
        sizeof(object_size), &property_size, 0));

    // 显式提供 Win7 兼容的哈希对象缓冲区，其生命周期必须长于哈希句柄。
    std::vector<std::byte> object(object_size);
    wil::unique_bcrypt_hash hash;
    THROW_IF_NTSTATUS_FAILED(::BCryptCreateHash(
        algorithm.get(), hash.put(), reinterpret_cast<PUCHAR>(object.data()),
        object_size, nullptr, 0, 0));
    if (!data.empty()) {
        THROW_IF_NTSTATUS_FAILED(::BCryptHashData(
            hash.get(), const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data.data())),
            static_cast<ULONG>(data.size()), 0));
    }
    std::array<std::byte, 16> digest{};
    THROW_IF_NTSTATUS_FAILED(::BCryptFinishHash(
        hash.get(), reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()), 0));
    return digest;
}

std::wstring Base64Encode(std::span<const std::byte> data) {
    // 标准 Base64，禁止插入换行；Hash 会作为一个 REG_SZ 值直接写入 UserChoice。
    // 不沿用原阅读器隐藏常量字符串的自定义字母表。
    if (data.empty()) {
        return {};
    }
    if (data.size() > std::numeric_limits<DWORD>::max()) {
        throw std::length_error("Base64 input is too large for CryptBinaryToStringW");
    }
    const auto* bytes = reinterpret_cast<const BYTE*>(data.data());
    const auto size = static_cast<DWORD>(data.size());
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD length = 0;
    THROW_IF_WIN32_BOOL_FALSE(::CryptBinaryToStringW(bytes, size, flags, nullptr, &length));
    std::wstring encoded(length, L'\0');
    THROW_IF_WIN32_BOOL_FALSE(::CryptBinaryToStringW(bytes, size, flags, encoded.data(), &length));
    if (!encoded.empty() && encoded.back() == L'\0') {
        encoded.pop_back();
    }
    return encoded;
}

} // namespace dfa::crypto
