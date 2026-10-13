"""Authored by Codex. Read existing cached signing levels; never set them."""
import ctypes as ct
from ctypes import wintypes as wt
import json
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[2]
    kernel = ct.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ct.c_void_p,
                                   wt.DWORD, wt.DWORD, wt.HANDLE]
    kernel.CreateFileW.restype = wt.HANDLE
    kernel.CloseHandle.argtypes = [wt.HANDLE]
    kernel.CloseHandle.restype = wt.BOOL
    kernel.GetCachedSigningLevel.argtypes = [wt.HANDLE, ct.POINTER(wt.DWORD),
        ct.POINTER(wt.DWORD), ct.POINTER(wt.BYTE), ct.POINTER(wt.DWORD), ct.POINTER(wt.DWORD)]
    kernel.GetCachedSigningLevel.restype = wt.BOOL
    targets = {
        "msbuild": Path(r"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"),
        "python": Path(r"C:\Users\ZACK\AppData\Local\Programs\Python\Python313\python.exe"),
        "native_exe": root / "out/build/windows-x64-v143/Release/DefaultFileAssociation.exe",
        "mshta": Path(r"C:\Windows\System32\mshta.exe"),
    }
    levels = {0: "Unchecked", 1: "Unsigned", 4: "Authenticode", 8: "Microsoft", 12: "Windows", 14: "WindowsTCB"}
    report = {"read_only": True, "limitation": "Cached file metadata, not a live UCPD branch trace.", "files": []}
    for label, path in targets.items():
        entry = {"label": label, "path": str(path)}
        handle = kernel.CreateFileW(str(path), 0x80000000, 7, None, 3, 0, None)
        if handle == ct.c_void_p(-1).value:
            entry["open_error"] = ct.get_last_error()
        else:
            try:
                flags, level, algorithm, size = wt.DWORD(), wt.DWORD(), wt.DWORD(), wt.DWORD(64)
                thumbprint = (wt.BYTE * 64)()
                ok = kernel.GetCachedSigningLevel(handle, ct.byref(flags), ct.byref(level),
                    thumbprint, ct.byref(size), ct.byref(algorithm))
                if ok:
                    entry.update(flags=flags.value, signing_level=level.value,
                                 signing_level_name=levels.get(level.value, "Other"))
                else:
                    entry["query_error"] = ct.get_last_error()
            finally:
                kernel.CloseHandle(handle)
        report["files"].append(entry)
    (root / "research/evidence/signing-level-probe.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
