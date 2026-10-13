#include "registry.h"

#include <windows.h>
#include <sddl.h>
#include <shlwapi.h>
#include <wil/resource.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void Run() {
    // All writes use a newly created private key, never real file associations.
    const std::wstring path = L"Software\\DefaultFileAssociationTest-" +
        std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
    wil::unique_hkey root;
    DWORD disposition = 0;
    Require(::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                               REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                               root.put(), &disposition) == ERROR_SUCCESS,
            "Create private test key");
    Require(disposition == REG_CREATED_NEW_KEY, "Never reuse an existing key");
    auto cleanup = wil::scope_exit([&] {
        root.reset();
        ::RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    });

    using namespace dfa::registry;
    const std::wstring key = L"Unicode\\文档";
    Require(SetString(root.get(), key, L"", L"中文默认值").has_value(), "Write Unicode default value");
    auto unicode = ReadString(root.get(), key, L"");
    Require(unicode && *unicode == L"中文默认值", "Read Unicode default value");
    Require(SetString(root.get(), key, L"Empty", L"").has_value(), "Write empty string");
    auto empty = ReadString(root.get(), key, L"Empty");
    Require(empty && empty->empty(), "Empty string stays distinct from missing value");
    const std::wstring long_value(8192, L'文');
    Require(SetString(root.get(), key, L"Long", long_value).has_value(), "Write long string");
    auto read_long = ReadString(root.get(), key, L"Long");
    Require(read_long && *read_long == long_value, "WIL dynamic buffer does not truncate");
    Require(LastWriteTime(root.get(), key).has_value(), "Read key timestamp");

    Require(SetEmptyValue(root.get(), key, L"OpenWith").has_value(), "Write OpenWithProgids value");
    DWORD type = REG_SZ;
    DWORD bytes = 1;
    Require(::RegGetValueW(root.get(), key.c_str(), L"OpenWith", RRF_RT_ANY,
                           &type, nullptr, &bytes) == ERROR_SUCCESS && type == REG_NONE && bytes == 0,
            "OpenWithProgids keeps zero-byte REG_NONE type");
    Require(!ReadString(root.get(), key, L"OpenWith"), "Non-string values are rejected");
    Require(DeleteValue(root.get(), key, L"Long").has_value(), "Delete value");
    auto missing = ReadString(root.get(), key, L"Long");
    Require(!missing && missing.error().status == ERROR_FILE_NOT_FOUND, "Missing value reports error");
    Require(DeleteValue(root.get(), key, L"Long").has_value(), "Deleting missing value is harmless");
    Require(DeleteValue(root.get(), L"MissingKey", L"Absent").has_value(), "Missing key deletion");
    Require(DeleteTree(root.get(), L"Unicode").has_value(), "Recursive cleanup");
    Require(!LastWriteTime(root.get(), key), "Deleted child key is absent");
    Require(DeleteTree(root.get(), L"Unicode").has_value(), "Recursive cleanup is idempotent");

    // Reproduce UserChoice's deny-SET_VALUE ACL without touching real defaults.
    Require(SetString(root.get(), L"Restricted", L"ProgId", L"Old").has_value(), "Seed restricted key");
    auto cleanup_restricted = wil::scope_exit([&] { ::RegDeleteKeyW(root.get(), L"Restricted"); });
    wil::unique_hkey restricted;
    Require(::RegOpenKeyExW(root.get(), L"Restricted", 0, WRITE_DAC, restricted.put()) == ERROR_SUCCESS,
            "Open test ACL");
    wil::unique_hlocal_security_descriptor descriptor;
    Require(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(D;;0x2;;;WD)(A;;KA;;;WD)", SDDL_REVISION_1, descriptor.put(), nullptr) != FALSE,
            "Create deny-SET_VALUE ACL");
    Require(::RegSetKeySecurity(restricted.get(), DACL_SECURITY_INFORMATION, descriptor.get()) == ERROR_SUCCESS,
            "Apply test ACL");
    restricted.reset();
    Require(::SHDeleteKeyW(root.get(), L"Restricted") == ERROR_ACCESS_DENIED, "Reproduce error 5");
    Require(DeleteTree(root.get(), L"Restricted").has_value(), "Delete leaf using existing DELETE rights");
    Require(!LastWriteTime(root.get(), L"Restricted"), "Restricted key removed");
}

} // namespace

int main() {
    try {
        Run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Registry test failure: %s\n", error.what());
        return 1;
    }
}
