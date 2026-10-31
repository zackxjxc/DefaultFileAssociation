"""Authored by Codex. Sequential controlled host tests; no UI or elevation."""
from datetime import datetime
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import uuid
import winreg


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", nargs="+", required=True, choices=["installutil", "regsvr32", "odbcconf", "regasm"],
                        help="Explicit host selection required. Workflow execution removed after Defender detection.")
    options = parser.parse_args()
    # Process-local inherited error handling prevents crash dialogs in batch tests.
    ctypes.WinDLL("kernel32").SetErrorMode(0x0001 | 0x0002 | 0x8000)
    root = Path(__file__).resolve().parents[2]
    logs = root / "research/evidence/host-probes"
    logs.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8]
    native_dll = root / "out/build/windows-x64-v143/Release/PdfAssociationHostProbe.dll"
    managed_dll = root / "research/host-probes/bin/ManagedHostProbe.dll"
    checker = root / "out/build/windows-x64-v143/Release/DefaultFileAssociation.exe"
    prefix = r"C:\Windows\Microsoft.NET\Framework64\v4.0.30319"
    response = root / "research/host-probes/odbc-probe.rsp"
    response.write_text('REGSVR "' + str(native_dll) + '"\n', encoding="utf-8", newline="\n")
    plans = [
        ("installutil", prefix + r"\InstallUtil.exe", ["/LogFile=", "/LogToConsole=false", "/U", str(managed_dll)], "json"),
        ("regsvr32", r"C:\Windows\System32\regsvr32.exe", ["/s", str(native_dll)], "log"),
        ("odbcconf", r"C:\Windows\System32\odbcconf.exe", ["/S", "/F", str(response)], "log"),
        ("regasm", prefix + r"\RegAsm.exe", ["/nologo", "/unregister", str(managed_dll)], "json"),
    ]
    if options.only:
        plans = [p for p in plans if p[0] in options.only]
    # Unregister mode may only touch the never-registered experiment identities.
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        for key in (r"Software\Classes\CLSID\{2FDE0A75-778F-4D31-B95C-0DBEC4D5B2A3}",
                    r"Software\Classes\PdfAssociationRegistrationProbe"):
            try:
                existing = winreg.OpenKey(hive, key, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
            except FileNotFoundError:
                continue
            else:
                existing.Close()
                raise RuntimeError("Experiment COM identity already exists; refusing unregister")
    def invoke(executable, arguments, log, environment=None):
        command = [str(executable), *arguments]
        try:
            result = subprocess.run(command, capture_output=True, env=environment, timeout=30,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            log.write_bytes(result.stdout + result.stderr)
            return result.returncode
        except subprocess.TimeoutExpired as error:
            log.write_bytes((error.stdout or b"") + (error.stderr or b""))
            return "timeout"
    def check(label):
        return invoke(checker, ["--latest-native-probe", "--ext=.pdf", "--prog-id=samplepdfreader.pdf", "--check"],
                      logs / (stamp + "-" + label + "-independent-check.log"))
    if check("initial") != 0:
        raise RuntimeError("Initial PDF hash/Shell validation failed; no host write tests attempted")
    summary_file = root / "research/evidence/alternate-host-results.json"
    summary = json.loads(summary_file.read_text(encoding="utf-8")) if summary_file.is_file() else {"hosts": []}
    selected = {p[0] for p in plans}
    summary["hosts"] = [h for h in summary["hosts"] if h["name"] not in selected]
    summary.update(stamp=stamp, computer_use_used=False, pdf_opened=False, complete=False)
    for name, executable, arguments, kind in plans:
        host = {"name": name, "executable": executable, "stamp": stamp, "phases": []}
        summary["hosts"].append(host)
        for mode in ("read", "write"):
            report = logs / (stamp + "-" + name + "-" + mode + "." + kind)
            environment = os.environ.copy()
            environment["DFA_HOST_PROBE_REPORT"] = str(report)
            environment["DFA_HOST_PROBE_MODE"] = mode
            exit_code = invoke(executable, arguments, logs / (stamp + "-" + name + "-" + mode + "-console.log"), environment)
            payload = None
            if report.is_file():
                text = report.read_text(encoding="utf-8")
                payload = json.loads(text) if kind == "json" else dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
            success = bool(payload) and (payload.get("write_probe_success") is True or payload.get("probe_success") == "true")
            independent = check(name + "-" + mode)
            phase = {"mode": mode, "host_exit_code": exit_code, "callback_executed": payload is not None,
                     "callback_success": success, "independent_check_exit_code": independent,
                     "report": str(report), "payload": payload}
            host["phases"].append(phase)
            summary_file.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
            print(name, mode, "host exit:", exit_code, "callback:", payload is not None,
                  "success:", success, "independent check:", independent, flush=True)
            if independent != 0:
                known_host = prefix + r"\MSBuild.exe"
                recovery = invoke(known_host, [str(root / "research/scripts/msbuild-latest-probe.proj"),
                    "/ToolsVersion:4.0", "/nologo", "/v:minimal", "/p:AttemptSet=true", "/p:PermitHashRepair=true",
                    "/p:ReportFile=" + str(logs / (stamp + "-recovery.json"))], logs / (stamp + "-recovery-console.log"))
                summary["recovery_exit_code"] = recovery
                summary["recovery_check_exit_code"] = check("recovery")
                summary_file.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
                raise RuntimeError("Stopped host tests after validation failure; recovery recorded")
            if mode == "read" and not success:
                host["write_skipped_reason"] = "Host callback did not pass read-only validation"
                break
    summary["complete"] = True
    summary_file.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("Report:", summary_file, flush=True)


if __name__ == "__main__":
    main()
