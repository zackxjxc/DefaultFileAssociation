// Authored by Codex. Standalone public-API PDF probe; listing never calls MakeDefault.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwchar>
#include <io.h>
#include <fcntl.h>

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    const bool apply = argc == 3 && std::wcscmp(argv[1], L"--make-default") == 0;
    if (!apply && !(argc == 2 && std::wcscmp(argv[1], L"--list") == 0)) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 3;
    int result = 4;
    {
        Microsoft::WRL::ComPtr<IEnumAssocHandlers> enumerator;
        hr = SHAssocEnumHandlers(L".pdf", ASSOC_FILTER_NONE, &enumerator);
        std::wprintf(L"enumeration_hresult=0x%08lx\n", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) {
            result = apply ? 5 : 0;
            for (unsigned index = 0; index < 128; ++index) {
                Microsoft::WRL::ComPtr<IAssocHandler> handler;
                ULONG fetched = 0;
                if (enumerator->Next(1, &handler, &fetched) != S_OK || fetched != 1) break;
                PWSTR name = nullptr, ui = nullptr;
                handler->GetName(&name);
                handler->GetUIName(&ui);
                void** table = *reinterpret_cast<void***>(handler.Get());
                HMODULE owner = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(table[7]), &owner);
                wchar_t module[MAX_PATH]{};
                if (owner) GetModuleFileNameW(owner, module, MAX_PATH);
                std::wprintf(L"handler[%u]=%ls; ui=%ls; MakeDefault_module=%ls; rva=0x%llx\n", index,
                    name ? name : L"", ui ? ui : L"", module,
                    static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(table[7]) - reinterpret_cast<ULONG_PTR>(owner)));
                if (apply && name && CompareStringOrdinal(name, -1, argv[2], -1, TRUE) == CSTR_EQUAL) {
                    std::wprintf(L"calling_public_MakeDefault=true\n");
                    std::fflush(stdout);
                    hr = handler->MakeDefault(ui ? ui : L"DocBox");
                    std::wprintf(L"MakeDefault_hresult=0x%08lx\n", static_cast<unsigned long>(hr));
                    result = SUCCEEDED(hr) ? 0 : 6;
                    CoTaskMemFree(name); CoTaskMemFree(ui);
                    break;
                }
                CoTaskMemFree(name); CoTaskMemFree(ui);
            }
        }
    }
    CoUninitialize();
    return result;
}
