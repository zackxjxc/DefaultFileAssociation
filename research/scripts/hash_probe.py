"""Validate the located UserChoiceLatest hash routine without registry writes.

This is a build-specific research probe, not a supported Windows API or setter.
The loaded system DLL must exactly match the binary analyzed by IDA.
"""
import ctypes as ct
from ctypes import wintypes as wt
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import winreg


class FILETIME(ct.Structure):
    _fields_ = [("low", wt.DWORD), ("high", wt.DWORD)]


class SYSTEMTIME(ct.Structure):
    _fields_ = [(name, wt.WORD) for name in
               ("year", "month", "weekday", "day", "hour", "minute", "second", "milliseconds")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--set-pdf", action="store_true", help="Attempt the authorized real .pdf change to samplepdfreader.pdf")
    options = parser.parse_args()
    project = Path(__file__).resolve().parents[2]
    inventory = json.loads((project / "research/evidence/windows-storage/inventory.json").read_text(encoding="utf-8"))
    system_dll = Path(r"C:\Windows\System32\windows.storage.dll")
    digest = hashlib.sha256(system_dll.read_bytes()).hexdigest()
    if digest != inventory["sha256"]:
        raise RuntimeError("System DLL differs from the exact analyzed build; refusing internal calls")
    identity = subprocess.run([r"C:\Windows\System32\whoami.exe", "/user", "/fo", "csv", "/nh"],
                              capture_output=True, check=True)
    sid = re.search(rb"S-1-5-[0-9-]+", identity.stdout).group().decode("ascii")
    extension = ".pdf"
    latest = rf"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\{extension}\UserChoiceLatest"
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, latest) as key:
        stored = winreg.QueryValueEx(key, "Hash")[0]
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, latest + r"\ProgId") as key:
        prog_id = winreg.QueryValueEx(key, "ProgId")[0]
        ticks = winreg.QueryInfoKey(key)[2]
    protected = rf"SOFTWARE\Microsoft\Windows\CurrentVersion\SystemProtectedUserData\{sid}\AnyoneRead\AppDefaults"
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, protected, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
        version = winreg.QueryValueEx(key, "HashVersion")[0]
    if version != 1:
        raise RuntimeError(f"Unvalidated hash version: {version}")
    kernel = ct.WinDLL("kernel32", use_last_error=True)
    kernel.FileTimeToSystemTime.argtypes = [ct.POINTER(FILETIME), ct.POINTER(SYSTEMTIME)]
    kernel.FileTimeToSystemTime.restype = wt.BOOL
    timestamp = SYSTEMTIME()
    filetime = FILETIME(ticks & 0xffffffff, ticks >> 32)
    if not kernel.FileTimeToSystemTime(ct.byref(filetime), ct.byref(timestamp)):
        raise ct.WinError(ct.get_last_error())
    ole = ct.WinDLL("ole32")
    ole.CoTaskMemFree.argtypes = [ct.c_void_p]
    ole.CoTaskMemFree.restype = None
    library = ct.WinDLL(str(system_dll))
    get_machine = ct.WINFUNCTYPE(ct.c_int32, ct.POINTER(ct.c_void_p))(library._handle + 0x6289FC)
    machine_pointer = ct.c_void_p()
    status = get_machine(ct.byref(machine_pointer))
    if status < 0:
        raise RuntimeError(f"Machine ID helper failed: HRESULT {status & 0xffffffff:#x}")
    try:
        machine_id = ct.wstring_at(machine_pointer)
    finally:
        ole.CoTaskMemFree(machine_pointer)
    generate = ct.WINFUNCTYPE(ct.c_int32, ct.c_uint32, ct.c_wchar_p, ct.c_wchar_p,
                             ct.c_wchar_p, ct.c_wchar_p, ct.POINTER(SYSTEMTIME),
                             ct.POINTER(ct.c_void_p))(library._handle + 0x628D98)
    def calculate(candidate):
        output = ct.c_void_p()
        hr = generate(version, machine_id, sid, extension, candidate, ct.byref(timestamp), ct.byref(output))
        if hr < 0:
            raise RuntimeError(f"Hash helper failed: HRESULT {hr & 0xffffffff:#x}")
        try:
            return ct.wstring_at(output)
        finally:
            ole.CoTaskMemFree(output)
    calculated = calculate(prog_id)
    target_calculated = calculate("samplepdfreader.pdf")
    report = {"dll_sha256": digest, "hash_routine_rva": "0x628D98",
              "machine_id_helper_rva": "0x6289FC", "hash_version": version,
              "extension": extension, "current_prog_id": prog_id,
              "stored_hash": stored, "calculated_hash": calculated,
              "current_hash_matches": stored == calculated,
              "target_prog_id": "samplepdfreader.pdf", "target_hash_with_existing_timestamp": target_calculated,
              "target_hash_differs": target_calculated != calculated,
              "registry_writes": 0,
              "limitation": "Internal function call for this exact DLL build; not an independent algorithm implementation or a write bypass."}
    if options.set_pdf:
        if stored != calculated:
            raise RuntimeError("Existing hash could not be reproduced; refusing writes")
        shell = ct.WinDLL("shell32")
        if shell.IsUserAnAdmin():
            raise RuntimeError("Research setter must run without elevation")
        report["set_attempted"] = True
        report["completed_registry_writes"] = 0
        stage = "open_latest_keys"
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, latest, 0, winreg.KEY_READ | winreg.KEY_SET_VALUE) as hash_key:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, latest + r"\ProgId", 0, winreg.KEY_READ | winreg.KEY_SET_VALUE) as prog_key:
                    stage = "set_latest_progid"
                    winreg.SetValueEx(prog_key, "ProgId", 0, winreg.REG_SZ, "samplepdfreader.pdf")
                    report["completed_registry_writes"] += 1
                    ticks_after = winreg.QueryInfoKey(prog_key)[2]
                    updated_time = FILETIME(ticks_after & 0xffffffff, ticks_after >> 32)
                    if not kernel.FileTimeToSystemTime(ct.byref(updated_time), ct.byref(timestamp)):
                        raise ct.WinError(ct.get_last_error())
                    new_hash = calculate("samplepdfreader.pdf")
                    stage = "set_latest_hash"
                    winreg.SetValueEx(hash_key, "Hash", 0, winreg.REG_SZ, new_hash)
                    report["completed_registry_writes"] += 1
                    report["set_write_success"] = True
        except OSError as error:
            report["set_write_success"] = False
            report["failure_stage"] = stage
            report["win32_error"] = error.winerror
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, latest) as key:
            report["after_hash"] = winreg.QueryValueEx(key, "Hash")[0]
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, latest + r"\ProgId") as key:
            report["after_prog_id"] = winreg.QueryValueEx(key, "ProgId")[0]
            report["latest_timestamp_unchanged"] = winreg.QueryInfoKey(key)[2] == ticks
        report["latest_values_unchanged"] = report["after_hash"] == stored and report["after_prog_id"] == prog_id
        report["registry_writes"] = report["completed_registry_writes"]
    report_name = "latest-set-probe.json" if options.set_pdf else "hash-probe.json"
    (project / "research/evidence" / report_name).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(report, indent=2))
    if stored != calculated:
        raise SystemExit(1)
    if options.set_pdf and not report.get("set_write_success", False):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
