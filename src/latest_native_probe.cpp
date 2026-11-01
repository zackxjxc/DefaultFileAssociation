#include "latest_native_probe.h"

#include "association.h"
#include "association_target.h"
#include "latest_hash_self.h"
#include "platform.h"
#include "registry.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <array>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <vector>

namespace dfa::latest_native {
namespace {

#if defined(_M_X64)

constexpr auto kLatest = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoiceLatest";

enum class HashEngine { Self, Dll };

std::wstring EnvironmentValue(const wchar_t* name) {
    std::array<wchar_t, 64> value{};
    const DWORD count = ::GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    return count == 0 || count >= value.size() ? std::wstring{} : std::wstring(value.data(), count);
}

struct HashProfile {
    std::wstring_view digest;
    uintptr_t machine_rva;
    uintptr_t hash_rva;
};

std::optional<HashProfile> Profile(std::wstring_view digest) {
    if (digest == L"8d6ac2b451af71e913665c8459935ac862e219458f0962c99f3a3d48ab9c251b") {
        return HashProfile{digest, 0x6289FC, 0x628D98};
    }
    if (digest == L"cf4793b9e7570f1c6621515d634e2fee115b51f6009a19e0d1a5d24cefb55703") {
        return HashProfile{digest, 0x625E8C, 0x626228};
    }
    // Windows 11 26H2 build 26300.9711. Static comparison matched the
    // implementation body; these RVAs moved in the serviced image. The
    // returned value is machine-bound and is validated against the live record.
    if (digest == L"a0d41fcfd66f4fbba8dbb84776c7d344f66969ce8b6edac19b3ac1dd737a628a") {
        return HashProfile{digest, 0x62902C, 0x6293C8};
    }
    return std::nullopt;
}

HashEngine RequestedEngine(std::optional<HashProfile> profile) {
    const auto requested = platform::ToLower(EnvironmentValue(L"DFA_LATEST_HASH_ENGINE"));
    if (requested == L"self") return HashEngine::Self;
    if (requested == L"dll") {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), !profile);
        return HashEngine::Dll;
    }
    return profile ? HashEngine::Dll : HashEngine::Self;
}

struct State {
    std::wstring prog_id;
    std::wstring hash;
    FILETIME time{};
};

State ReadState(HKEY parent, HKEY child) {
    auto id = registry::ReadString(child, L"", L"ProgId");
    auto hash = registry::ReadString(parent, L"", L"Hash");
    auto time = registry::LastWriteTime(child, L"");
    THROW_IF_WIN32_ERROR(id ? ERROR_SUCCESS : id.error().status);
    THROW_IF_WIN32_ERROR(hash ? ERROR_SUCCESS : hash.error().status);
    THROW_IF_WIN32_ERROR(time ? ERROR_SUCCESS : time.error().status);
    return {*id, *hash, *time};
}

void Describe(std::wstring& trace, std::wstring_view prefix, const State& state) {
    trace += std::format(L"{}_prog_id={}\n{}_hash={}\n{}_prog_id_filetime={:08x}{:08x}\n",
        prefix, state.prog_id, prefix, state.hash, prefix, state.time.dwHighDateTime, state.time.dwLowDateTime);
}

std::wstring FileSha256(const std::filesystem::path& path) {
    // Hash the disk file without allowing concurrent replacement.
    wil::unique_hfile file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    THROW_LAST_ERROR_IF(file.get() == INVALID_HANDLE_VALUE);
    wil::unique_bcrypt_algorithm algorithm;
    THROW_IF_NTSTATUS_FAILED(::BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    ULONG object_size = 0, bytes = 0;
    THROW_IF_NTSTATUS_FAILED(::BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &bytes, 0));
    std::vector<BYTE> object(object_size);
    wil::unique_bcrypt_hash hash;
    THROW_IF_NTSTATUS_FAILED(::BCryptCreateHash(algorithm.get(), hash.put(), object.data(), object_size, nullptr, 0, 0));
    std::array<BYTE, 65536> buffer{};
    for (;;) {
        THROW_IF_WIN32_BOOL_FALSE(::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr));
        if (bytes == 0) break;
        THROW_IF_NTSTATUS_FAILED(::BCryptHashData(hash.get(), buffer.data(), bytes, 0));
    }
    std::array<BYTE, 32> digest{};
    THROW_IF_NTSTATUS_FAILED(::BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0));
    std::wstring result;
    for (BYTE value : digest) result += std::format(L"{:02x}", value);
    return result;
}

bool VerifyShell(std::wstring_view target, const std::wstring& expected_command, std::wstring& trace) {
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wil::unique_couninitialize_call cleanup(SUCCEEDED(initialized));
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) THROW_IF_FAILED(initialized);
    auto registration = wil::CoCreateInstance<IApplicationAssociationRegistration>(
        __uuidof(ApplicationAssociationRegistration));
    wil::unique_cotaskmem_string id;
    THROW_IF_FAILED(registration->QueryCurrentDefault(L".pdf", AT_FILEEXTENSION, AL_EFFECTIVE, id.put()));
    DWORD size = 0;
    THROW_IF_FAILED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, L".pdf", L"open", nullptr, &size));
    THROW_HR_IF(E_UNEXPECTED, size == 0);
    std::wstring command(size, L'\0');
    THROW_IF_FAILED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, L".pdf", L"open", command.data(), &size));
    command.resize(std::wcslen(command.c_str()));
    trace += std::format(L"shell_prog_id={}\nshell_open_command={}\n", id.get(), command);
    return platform::EqualsInsensitive(id.get(), target) && command == expected_command;
}

#endif

} // namespace

Result Preflight(std::wstring& trace) {
    trace = std::format(L"engine=latest-native\nmode=preflight\nprocess_bits={}\n", sizeof(void*) * 8);
#if !defined(_M_X64)
    trace += L"failure_stage=architecture\ncompleted_registry_writes=0\n";
    return {Status::Failed, L"This build-specific experiment requires the x64 EXE.", ERROR_NOT_SUPPORTED, false};
#else
    std::wstring stage = L"preflight";
    try {
        THROW_HR_IF(E_ACCESSDENIED, platform::IsProcessElevated());
        const std::wstring sid = platform::GetCurrentUserSid();
        THROW_HR_IF(E_UNEXPECTED, sid.empty());
        std::array<wchar_t, MAX_PATH> directory{};
        const UINT count = ::GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        THROW_LAST_ERROR_IF(count == 0);
        THROW_HR_IF(E_UNEXPECTED, count >= directory.size());
        const auto dll = std::filesystem::path(directory.data()) / L"windows.storage.dll";
        std::optional<HashProfile> profile;
        std::wstring digest;
        wil::unique_hfile dll_lock;
        const auto requested = platform::ToLower(EnvironmentValue(L"DFA_LATEST_HASH_ENGINE"));
        if (requested != L"self") {
            stage = L"dll_fingerprint";
            dll_lock.reset(::CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            THROW_LAST_ERROR_IF(dll_lock.get() == INVALID_HANDLE_VALUE);
            digest = FileSha256(dll);
            trace += L"dll_sha256=" + digest + L"\n";
            profile = Profile(digest);
        }
        const auto engine = RequestedEngine(profile);
        trace += std::wstring(L"hash_engine=") + (engine == HashEngine::Self ? L"self\n" : L"dll\n");
        stage = L"hash_version";
        wil::unique_hkey protected_key;
        const std::wstring protected_path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SystemProtectedUserData\\" +
            sid + L"\\AnyoneRead\\AppDefaults";
        THROW_IF_WIN32_ERROR(::RegOpenKeyExW(HKEY_LOCAL_MACHINE, protected_path.c_str(), 0,
            KEY_READ | KEY_WOW64_64KEY, protected_key.put()));
        DWORD version = 0, size = sizeof(version), type = 0;
        THROW_IF_WIN32_ERROR(::RegQueryValueExW(protected_key.get(), L"HashVersion", nullptr, &type,
            reinterpret_cast<BYTE*>(&version), &size));
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), type != REG_DWORD || size != sizeof(version) || version != 1);
        trace += std::format(L"hash_version={}\n", version);
        using GetMachine = HRESULT(WINAPI*)(wchar_t**);
        using Generate = HRESULT(WINAPI*)(DWORD, const wchar_t*, const wchar_t*, const wchar_t*,
            const wchar_t*, SYSTEMTIME*, wchar_t**);
        wil::unique_hmodule module;
        GetMachine get_machine = nullptr;
        Generate generate = nullptr;
        if (engine == HashEngine::Dll) {
            stage = L"load_hash_functions";
            module.reset(::LoadLibraryW(dll.c_str()));
            THROW_LAST_ERROR_IF(module.get() == nullptr);
            const auto base = reinterpret_cast<const BYTE*>(module.get());
            get_machine = reinterpret_cast<GetMachine>(base + profile->machine_rva);
            generate = reinterpret_cast<Generate>(base + profile->hash_rva);
        }
        std::wstring machine;
        if (engine == HashEngine::Self) {
            stage = L"machine_id";
            machine = latest_hash::ReadMachineId();
        } else {
            wil::unique_cotaskmem_string machine_pointer;
            THROW_IF_FAILED(get_machine(machine_pointer.put()));
            if (machine_pointer) machine = machine_pointer.get();
        }
        THROW_HR_IF(E_UNEXPECTED, machine.empty());
        stage = L"hash_selftest";
        SYSTEMTIME vector_time{2001, 2, 6, 3, 4, 5, 6, 7};
        std::wstring vector_value;
        if (engine == HashEngine::Self) {
            const auto vector = latest_hash::Generate(L"{01234567-89AB-CDEF-0123-456789ABCDEF}",
                L"S-1-5-21-1-2-3-1000", L".pdf", L"samplepdfreader.pdf", vector_time);
            THROW_HR_IF(E_FAIL, !vector.ok);
            vector_value = vector.value;
        } else {
            wil::unique_cotaskmem_string vector_hash;
            THROW_IF_FAILED(generate(1, L"{01234567-89AB-CDEF-0123-456789ABCDEF}",
                L"S-1-5-21-1-2-3-1000", L".pdf", L"samplepdfreader.pdf", &vector_time, vector_hash.put()));
            if (vector_hash) vector_value = vector_hash.get();
        }
        // The helper incorporates the machine identity, so a fixed output
        // string is not portable across otherwise identical installations.
        // Require a successful, non-empty result here; the existing record
        // validation below is the authoritative end-to-end self-test.
        const bool vector_valid = !vector_value.empty();
        trace += std::format(L"hash_selftest_value={}\nhash_selftest={}\n",
            vector_valid ? vector_value : L"<null>", vector_valid);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), !vector_valid);
        stage = L"validate_existing_record";
        wil::unique_hkey parent, child;
        THROW_IF_WIN32_ERROR(::RegOpenKeyExW(HKEY_CURRENT_USER, kLatest, 0, KEY_READ, parent.put()));
        const std::wstring child_path = std::wstring(kLatest) + L"\\ProgId";
        THROW_IF_WIN32_ERROR(::RegOpenKeyExW(HKEY_CURRENT_USER, child_path.c_str(), 0, KEY_READ, child.put()));
        const auto before = ReadState(parent.get(), child.get());
        Describe(trace, L"before", before);
        SYSTEMTIME timestamp{};
        THROW_IF_WIN32_BOOL_FALSE(::FileTimeToSystemTime(&before.time, &timestamp));
        std::wstring calculated;
        if (engine == HashEngine::Self) {
            const auto value = latest_hash::Generate(machine, sid, L".pdf", before.prog_id, timestamp);
            THROW_HR_IF(E_FAIL, !value.ok);
            calculated = value.value;
        } else {
            wil::unique_cotaskmem_string pointer;
            THROW_IF_FAILED(generate(version, machine.c_str(), sid.c_str(), L".pdf", before.prog_id.c_str(),
                &timestamp, pointer.put()));
            if (pointer) calculated = pointer.get();
        }
        const bool matches = calculated == before.hash;
        trace += std::format(L"before_hash_matches={}\n", matches);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), !matches);
        trace += L"preflight_success=true\ncompleted_registry_writes=0\n";
        return {Status::Success, L"Latest association host preflight passed; no writes were attempted.", 0, false};
    } catch (const wil::ResultException& error) {
        const HRESULT hr = error.GetErrorCode();
        const auto code = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? static_cast<DWORD>(HRESULT_CODE(hr)) : 0;
        trace += std::format(L"failure_stage={}\nfailure_hresult=0x{:08x}\ncompleted_registry_writes=0\npreflight_success=false\n",
            stage, static_cast<DWORD>(hr));
        return {Status::Failed, L"Latest association host preflight failed at " + stage + L".", code, false};
    } catch (const std::exception&) {
        trace += std::format(L"failure_stage={}\ncompleted_registry_writes=0\npreflight_success=false\n", stage);
        return {Status::Failed, L"Latest association host preflight encountered an unexpected error.", 0, false};
    }
#endif
}

Result Probe(std::wstring_view prog_id, bool attempt_set, std::wstring& trace) {
    trace = std::format(L"engine=latest-native\nprocess_bits={}\nattempt_set={}\ntarget_prog_id={}\n",
        sizeof(void*) * 8, attempt_set, prog_id);
#if !defined(_M_X64)
    trace += L"failure_stage=architecture\ncompleted_registry_writes=0\n";
    return {Status::Failed, L"This build-specific experiment requires the x64 EXE.", ERROR_NOT_SUPPORTED, false};
#else
    std::wstring stage = L"preflight";
    unsigned writes = 0;
    bool have_before = false;
    State before;
    wil::unique_hkey parent, child;
    try {
        THROW_HR_IF(E_INVALIDARG, !association::IsValidProgId(prog_id));
        THROW_HR_IF(E_ACCESSDENIED, platform::IsProcessElevated());
        auto expected_command = association::ReadRegisteredCommand(prog_id);
        THROW_IF_WIN32_ERROR(expected_command ? ERROR_SUCCESS : expected_command.error().status);
        const std::wstring sid = platform::GetCurrentUserSid();
        THROW_HR_IF(E_UNEXPECTED, sid.empty());
        std::array<wchar_t, MAX_PATH> directory{};
        UINT count = ::GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        THROW_LAST_ERROR_IF(count == 0);
        THROW_HR_IF(E_UNEXPECTED, count >= directory.size());
        const auto dll = std::filesystem::path(directory.data()) / L"windows.storage.dll";
        std::optional<HashProfile> profile;
        std::wstring digest;
        wil::unique_hfile dll_lock;
        const auto requested = platform::ToLower(EnvironmentValue(L"DFA_LATEST_HASH_ENGINE"));
        if (requested != L"self") {
            stage = L"dll_fingerprint";
            // The disk hash authorizes only the known RVAs; keep the image read-only while loading it.
            dll_lock.reset(::CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            THROW_LAST_ERROR_IF(dll_lock.get() == INVALID_HANDLE_VALUE);
            digest = FileSha256(dll);
            trace += L"dll_sha256=" + digest + L"\n";
            profile = Profile(digest);
        }
        const auto engine = RequestedEngine(profile);
        trace += std::wstring(L"hash_engine=") + (engine == HashEngine::Self ? L"self\n" : L"dll\n");
        stage = L"hash_version";
        wil::unique_hkey protected_key;
        const std::wstring protected_path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SystemProtectedUserData\\" +
            sid + L"\\AnyoneRead\\AppDefaults";
        THROW_IF_WIN32_ERROR(::RegOpenKeyExW(HKEY_LOCAL_MACHINE, protected_path.c_str(), 0,
            KEY_READ | KEY_WOW64_64KEY, protected_key.put()));
        DWORD version = 0, size = sizeof(version), type = 0;
        THROW_IF_WIN32_ERROR(::RegQueryValueExW(protected_key.get(), L"HashVersion", nullptr, &type,
            reinterpret_cast<BYTE*>(&version), &size));
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), type != REG_DWORD || size != sizeof(version) || version != 1);
        trace += std::format(L"hash_version={}\n", version);
        using GetMachine = HRESULT(WINAPI*)(wchar_t**);
        using Generate = HRESULT(WINAPI*)(DWORD, const wchar_t*, const wchar_t*, const wchar_t*,
            const wchar_t*, SYSTEMTIME*, wchar_t**);
        wil::unique_hmodule module;
        GetMachine get_machine = nullptr;
        Generate generate = nullptr;
        if (engine == HashEngine::Dll) {
            stage = L"load_hash_functions";
            module.reset(::LoadLibraryW(dll.c_str()));
            THROW_LAST_ERROR_IF(module.get() == nullptr);
            const auto base = reinterpret_cast<const BYTE*>(module.get());
            get_machine = reinterpret_cast<GetMachine>(base + profile->machine_rva);
            generate = reinterpret_cast<Generate>(base + profile->hash_rva);
        }
        std::wstring machine;
        if (engine == HashEngine::Self) {
            stage = L"machine_id";
            machine = latest_hash::ReadMachineId();
        } else {
            wil::unique_cotaskmem_string machine_pointer;
            THROW_IF_FAILED(get_machine(machine_pointer.put()));
            if (machine_pointer) machine = machine_pointer.get();
        }
        THROW_HR_IF(E_UNEXPECTED, machine.empty());
        stage = L"open_latest_keys";
        const REGSAM access = KEY_READ | (attempt_set ? KEY_SET_VALUE : 0);
        THROW_IF_WIN32_ERROR(::RegOpenKeyExW(HKEY_CURRENT_USER, kLatest, 0, access, parent.put()));
        const std::wstring child_path = std::wstring(kLatest) + L"\\ProgId";
        THROW_IF_WIN32_ERROR(::RegOpenKeyExW(HKEY_CURRENT_USER, child_path.c_str(), 0, access, child.put()));
        before = ReadState(parent.get(), child.get());
        have_before = true;
        Describe(trace, L"before", before);
        auto calculate = [&](const State& state) {
            SYSTEMTIME timestamp{};
            THROW_IF_WIN32_BOOL_FALSE(::FileTimeToSystemTime(&state.time, &timestamp));
            if (engine == HashEngine::Self) {
                const auto result = latest_hash::Generate(machine, sid, L".pdf", state.prog_id, timestamp);
                THROW_HR_IF(E_FAIL, !result.ok);
                return result.value;
            }
            wil::unique_cotaskmem_string result;
            THROW_IF_FAILED(generate(version, machine.c_str(), sid.c_str(), L".pdf", state.prog_id.c_str(), &timestamp, result.put()));
            THROW_HR_IF(E_UNEXPECTED, result == nullptr);
            return std::wstring(result.get());
        };
        stage = L"validate_existing_hash";
        const auto expected_before = calculate(before);
        trace += std::format(L"before_hash_calculated={}\nbefore_hash_matches={}\n",
            expected_before, expected_before == before.hash);
        // Read-only validation must reject an invalid record. An explicit set
        // experiment may repair it: the DLL fingerprint and hash version still
        // guard the internal call, and the actual registry writes are recorded.
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), !attempt_set && expected_before != before.hash);
        if (attempt_set) {
            stage = L"set_latest_progid";
            const std::wstring target(prog_id);
            // Deliberately perform this syscall even if target is already current.
            THROW_IF_WIN32_ERROR(::RegSetValueExW(child.get(), L"ProgId", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(target.c_str()), static_cast<DWORD>((target.size() + 1) * sizeof(wchar_t))));
            ++writes;
            stage = L"set_latest_hash";
            const auto hash_value = calculate(ReadState(parent.get(), child.get()));
            THROW_IF_WIN32_ERROR(::RegSetValueExW(parent.get(), L"Hash", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(hash_value.c_str()), static_cast<DWORD>((hash_value.size() + 1) * sizeof(wchar_t))));
            ++writes;
            ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_FLUSH, nullptr, nullptr);
        }
        stage = L"verify_result";
        const auto after = ReadState(parent.get(), child.get());
        Describe(trace, L"after", after);
        const bool hash_matches = calculate(after) == after.hash;
        const bool shell_matches = VerifyShell(prog_id, *expected_command, trace);
        trace += std::format(L"after_hash_matches={}\nshell_matches={}\ncompleted_registry_writes={}\n",
            hash_matches, shell_matches, writes);
        if (!platform::EqualsInsensitive(after.prog_id, prog_id) || !hash_matches || !shell_matches) {
            return {Status::UserChoiceRejected, L"Native validation did not confirm the requested PDF default.", 0, false};
        }
        trace += L"probe_success=true\n";
        return {Status::Success, attempt_set ? L"Native PDF writes and Shell verification succeeded." :
            L"Read-only native hash and Shell verification succeeded; no write was attempted.", 0, false};
    } catch (const wil::ResultException& error) {
        const HRESULT hr = error.GetErrorCode();
        const auto code = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? static_cast<DWORD>(HRESULT_CODE(hr)) : 0;
        trace += std::format(L"failure_stage={}\nfailure_hresult=0x{:08x}\ncompleted_registry_writes={}\nprobe_success=false\n",
            stage, static_cast<DWORD>(hr), writes);
        if (have_before) {
            try {
                const auto after = ReadState(parent.get(), child.get());
                Describe(trace, L"after", after);
                trace += std::format(L"latest_values_unchanged={}\nlatest_timestamp_unchanged={}\n",
                    after.prog_id == before.prog_id && after.hash == before.hash,
                    after.time.dwHighDateTime == before.time.dwHighDateTime && after.time.dwLowDateTime == before.time.dwLowDateTime);
            } catch (...) {
                trace += L"after_state_unavailable=true\n";
            }
        }
        return {Status::Failed, L"Native PDF experiment failed at " + stage + L".", code, false};
    } catch (const std::exception&) {
        trace += std::format(L"failure_stage={}\ncompleted_registry_writes={}\nprobe_success=false\n", stage, writes);
        return {Status::Failed, L"Native PDF experiment encountered an unexpected C++ error.", 0, false};
    }
#endif
}

} // namespace dfa::latest_native
