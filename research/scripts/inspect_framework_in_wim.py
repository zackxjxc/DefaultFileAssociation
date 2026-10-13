"""Authored by Codex. Inspect WIM using Windows WIMGAPI in read-only mode.

Extracts only named research files into a fresh workspace directory. It never
applies or mounts a Windows installation image and never runs extracted code.
"""
import ctypes as ct
from ctypes import wintypes as wt
from datetime import datetime
import hashlib
import json
from pathlib import Path
import uuid
import xml.etree.ElementTree as ET


def main():
    root = Path(__file__).resolve().parents[2]
    mount = json.loads((root / "research/evidence/iso-mount-state.json").read_text(encoding="utf-8"))
    image_path = mount["Volumes"][0]["Path"] + "sources\\install.wim"
    destination = root / "research/iso-inspection" / (datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    destination.mkdir(parents=True)
    temporary = destination / "temporary"
    temporary.mkdir()
    api = ct.WinDLL(r"C:\Windows\System32\wimgapi.dll", use_last_error=True)
    kernel = ct.WinDLL("kernel32", use_last_error=True)
    api.WIMCreateFile.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ct.POINTER(wt.DWORD)]
    api.WIMCreateFile.restype = wt.HANDLE
    api.WIMSetTemporaryPath.argtypes = [wt.HANDLE, wt.LPCWSTR]
    api.WIMSetTemporaryPath.restype = wt.BOOL
    api.WIMGetImageCount.argtypes = [wt.HANDLE]
    api.WIMGetImageCount.restype = wt.DWORD
    api.WIMLoadImage.argtypes = [wt.HANDLE, wt.DWORD]
    api.WIMLoadImage.restype = wt.HANDLE
    api.WIMGetImageInformation.argtypes = [wt.HANDLE, ct.POINTER(ct.c_void_p), ct.POINTER(wt.DWORD)]
    api.WIMGetImageInformation.restype = wt.BOOL
    api.WIMExtractImagePath.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD]
    api.WIMExtractImagePath.restype = wt.BOOL
    api.WIMCloseHandle.argtypes = [wt.HANDLE]
    api.WIMCloseHandle.restype = wt.BOOL
    kernel.LocalFree.argtypes = [ct.c_void_p]
    kernel.LocalFree.restype = ct.c_void_p
    disposition = wt.DWORD()
    wim = api.WIMCreateFile(image_path, 0x80000000, 3, 0, 0, ct.byref(disposition))
    if not wim or wim == ct.c_void_p(-1).value:
        raise ct.WinError(ct.get_last_error())
    report = {"iso": mount["ImagePath"], "wim": image_path, "read_only_wim": True,
              "extraction_directory": str(destination), "images": []}
    report_path = root / "research/evidence/iso-framework-inspection.json"
    targets = [
        r"\Windows\Microsoft.NET\Framework64\v4.0.30319\MSBuild.exe",
        r"\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe",
        r"\Windows\Microsoft.NET\Framework64\v4.0.30319\Microsoft.Build.Tasks.v4.0.dll",
        r"\Windows\Microsoft.NET\Framework\v4.0.30319\Microsoft.Build.Tasks.v4.0.dll",
        r"\Windows\Microsoft.NET\Framework64\v4.0.30319\Microsoft.Build.Utilities.v4.0.dll",
        r"\Windows\Microsoft.NET\Framework64\v4.0.30319\MSBuild.exe.config",
    ]
    def save():
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    try:
        if not api.WIMSetTemporaryPath(wim, str(temporary)):
            raise ct.WinError(ct.get_last_error())
        count = api.WIMGetImageCount(wim)
        report["image_count"] = count
        print("WIM images:", count, flush=True)
        for index in range(1, count + 1):
            image = api.WIMLoadImage(wim, index)
            if not image:
                raise ct.WinError(ct.get_last_error())
            try:
                pointer, length = ct.c_void_p(), wt.DWORD()
                if not api.WIMGetImageInformation(image, ct.byref(pointer), ct.byref(length)):
                    raise ct.WinError(ct.get_last_error())
                try:
                    xml = ct.string_at(pointer.value, length.value).decode("utf-16-le").lstrip("\ufeff").rstrip("\x00")
                finally:
                    kernel.LocalFree(pointer)
                tree = ET.fromstring(xml)
                entry = {"index": index, "name": tree.findtext("NAME"), "edition": tree.findtext("WINDOWS/EDITIONID"),
                         "version": {x.tag: x.text for x in tree.findall("WINDOWS/VERSION/*")}, "files": []}
                report["images"].append(entry)
                paths = list(targets)
                if index == 1:
                    paths += [r"\Windows\System32\windows.storage.dll", r"\Windows\System32\drivers\UCPD.sys"]
                for number, source in enumerate(paths):
                    folder = destination / f"index-{index}" / f"file-{number}"
                    folder.mkdir(parents=True)
                    ok = api.WIMExtractImagePath(image, source, str(folder), 0)
                    error = 0 if ok else ct.get_last_error()
                    extracted = [p for p in folder.rglob("*") if p.is_file()]
                    item = {"image_path": source, "extract_success": bool(ok), "error": error, "extracted": []}
                    for path in extracted:
                        item["extracted"].append({"path": str(path), "bytes": path.stat().st_size,
                                                   "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
                    entry["files"].append(item)
                    save()
                print("Image", index, json.dumps(entry["name"]), "files extracted:",
                      sum(x["extract_success"] for x in entry["files"]), "/", len(entry["files"]), flush=True)
            finally:
                api.WIMCloseHandle(image)
        report["complete"] = True
        save()
    finally:
        api.WIMCloseHandle(wim)
    print("Report:", report_path, flush=True)


if __name__ == "__main__":
    main()
