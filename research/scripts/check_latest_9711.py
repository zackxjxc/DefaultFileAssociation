"""Read-only UserChoiceLatest hash check for one statically matched DLL build.

Uses undocumented internal functions. This is a research check, not a setter
or an independent implementation of the algorithm. Run with 64-bit Python.
"""
import ctypes as ct
from ctypes import wintypes as wt
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import winreg

from hash_probe import FILETIME, SYSTEMTIME


def main():
    if ct.sizeof(ct.c_void_p) != 8:
        raise RuntimeError("64-bit Python is required")
    dll = Path(r"C:\Windows\System32\windows.storage.dll")
    digest = "a0d41fcfd66f4fbba8dbb84776c7d344f66969ce8b6edac19b3ac1dd737a628a"
    with dll.open("rb") as disk:
        data = disk.read()
        if hashlib.sha256(data).hexdigest() != digest:
            raise RuntimeError("Unsupported DLL fingerprint; refusing internal calls")
        library = ct.WinDLL(str(dll))
        mapped_pe = ct.c_uint32.from_address(library._handle + 0x3c).value
        disk_pe = struct.unpack_from("<I", data, 0x3c)[0]
        disk_stamp = struct.unpack_from("<I", data, disk_pe + 8)[0]
        if ct.c_uint32.from_address(library._handle + mapped_pe + 8).value != disk_stamp:
            raise RuntimeError("Mapped image differs from the disk build")
        ole = ct.WinDLL("ole32")
        ole.CoTaskMemFree.argtypes = [ct.c_void_p]
        ole.CoTaskMemFree.restype = None
        generate = ct.WINFUNCTYPE(ct.c_int32, ct.c_uint32, ct.c_wchar_p, ct.c_wchar_p,
                                 ct.c_wchar_p, ct.c_wchar_p, ct.POINTER(SYSTEMTIME),
                                 ct.POINTER(ct.c_void_p))(library._handle + 0x6293c8)

        def calculate(machine, sid, prog_id, timestamp):
            output = ct.c_void_p()
            hr = generate(1, machine, sid, ".pdf", prog_id, ct.byref(timestamp), ct.byref(output))
            try:
                if hr < 0 or not output.value:
                    raise RuntimeError(f"Hash generation failed: {hr & 0xffffffff:#x}")
                return ct.wstring_at(output)
            finally:
                ole.CoTaskMemFree(output)

        vector = calculate("{01234567-89AB-CDEF-0123-456789ABCDEF}",
                           "S-1-5-21-1-2-3-1000", "docboxtest.pdf",
                           SYSTEMTIME(2001, 2, 6, 3, 4, 5, 6, 7))
        if vector != "9rTXfFYM9CQ=":
            raise RuntimeError("Fixed hash vector did not match")
        identity = subprocess.run([r"C:\Windows\System32\whoami.exe", "/user", "/fo", "csv", "/nh"],
                                  capture_output=True, check=True)
        sid = re.search(rb"S-1-5-[0-9-]+", identity.stdout).group().decode("ascii")
        protected = rf"SOFTWARE\Microsoft\Windows\CurrentVersion\SystemProtectedUserData\{sid}\AnyoneRead\AppDefaults"
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, protected, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            version = winreg.QueryValueEx(key, "HashVersion")[0]
        if version != 1:
            raise RuntimeError(f"Unvalidated HashVersion: {version}")
        latest = r"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoiceLatest"
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, latest) as key:
            stored = winreg.QueryValueEx(key, "Hash")[0]
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, latest + r"\ProgId") as key:
            prog_id = winreg.QueryValueEx(key, "ProgId")[0]
            ticks = winreg.QueryInfoKey(key)[2]
        kernel = ct.WinDLL("kernel32", use_last_error=True)
        kernel.FileTimeToSystemTime.argtypes = [ct.POINTER(FILETIME), ct.POINTER(SYSTEMTIME)]
        kernel.FileTimeToSystemTime.restype = wt.BOOL
        timestamp = SYSTEMTIME()
        filetime = FILETIME(ticks & 0xffffffff, ticks >> 32)
        if not kernel.FileTimeToSystemTime(ct.byref(filetime), ct.byref(timestamp)):
            raise ct.WinError(ct.get_last_error())
        get_machine = ct.WINFUNCTYPE(ct.c_int32, ct.POINTER(ct.c_void_p))(library._handle + 0x62902c)
        machine = ct.c_void_p()
        hr = get_machine(ct.byref(machine))
        try:
            if hr < 0 or not machine.value:
                raise RuntimeError(f"Machine ID query failed: {hr & 0xffffffff:#x}")
            calculated = calculate(ct.wstring_at(machine), sid, prog_id, timestamp)
        finally:
            ole.CoTaskMemFree(machine)
    report = {"dll_sha256": digest, "machine_rva": "0x62902c", "hash_rva": "0x6293c8",
              "fixed_vector_matches": True, "hash_version": version, "prog_id": prog_id,
              "stored_hash": stored, "calculated_hash": calculated,
              "current_hash_matches": stored == calculated, "registry_writes": 0}
    print(json.dumps(report, indent=2))
    if not report["current_hash_matches"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
