"""Authored by Codex. Explicit, sequential PDF host tests from a temporary ProgID.

Keeps the same SamplePdfReader reader, preserves existing registration, and removes only
the newly created control registration after restoring samplepdfreader.pdf.
"""
import argparse
import ctypes
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import uuid
import winreg


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--only', nargs='+', required=True,
                        choices=['installutil', 'regsvr32', 'odbcconf', 'msiexec'])
    options = parser.parse_args()
    ctypes.WinDLL('kernel32').SetErrorMode(0x0001 | 0x0002 | 0x8000)
    root = Path(__file__).resolve().parents[2]
    folder = root / 'research/evidence/host-probes'
    folder.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8]
    control = 'samplepdfreader.control.' + uuid.uuid4().hex[:8]
    target = 'samplepdfreader.pdf'
    windows = Path(os.environ['SystemRoot'])
    framework = windows / 'Microsoft.NET/Framework64/v4.0.30319'
    checker = root / 'out/build/windows-x64-v143/Release/DefaultFileAssociation.exe'
    dll = root / 'out/build/windows-x64-host-research/Release/PdfAssociationHostProbe.dll'
    managed = root / 'research/host-probes/bin/ManagedHostProbe.dll'
    for file in [checker, dll, managed, framework / 'MSBuild.exe']:
        if not file.is_file():
            raise RuntimeError('Missing prerequisite: ' + str(file))
    latest = r'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoiceLatest\ProgId'

    def current():
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, latest) as key:
            return winreg.QueryValueEx(key, 'ProgId')[0]

    def invoke(command, label, env=None):
        result = subprocess.run([str(v) for v in command], capture_output=True, env=env,
                                timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
        (folder / (stamp + '-' + label + '-console.log')).write_bytes(result.stdout + result.stderr)
        return result.returncode

    def check(prog, label):
        return invoke([checker, '--msbuild', '--check', '--ext=.pdf', '--prog-id=' + prog], label) == 0

    def set_via_msbuild(prog, label):
        report = folder / (stamp + '-' + label + '.json')
        environment = os.environ.copy()
        environment.update(DFA_MSB_MODE='set', DFA_MSB_PROGID=prog, DFA_MSB_REPORT=str(report))
        code = invoke([framework / 'MSBuild.exe', root / 'src/msbuild_task.proj',
                       '/ToolsVersion:4.0', '/nologo', '/v:quiet', '/nodeReuse:false'], label, environment)
        if code != 0 or current() != prog or not check(prog, label + '-independent'):
            raise RuntimeError('MSBuild control/restore failed: ' + label)

    def defender(since):
        environment = os.environ.copy()
        environment['DFA_RESEARCH_AV_SINCE'] = since
        command = r"""[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); $queryError=$null; $events=@(); try { $events=@(Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-Windows Defender/Operational';Id=1116,1117;StartTime=[DateTime]::Parse($env:DFA_RESEARCH_AV_SINCE)} -ErrorAction Stop | Select-Object TimeCreated,Id,Message) } catch { if ($_.FullyQualifiedErrorId -notlike 'NoMatchingEventsFound*') { $queryError=$_.Exception.Message } }; @{Events=$events;QueryError=$queryError} | ConvertTo-Json -Depth 5 -Compress"""
        result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', command],
                                env=environment, capture_output=True, timeout=15,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode != 0:
            return {'Events': [], 'QueryError': 'PowerShell query exit ' + str(result.returncode)}
        return json.loads(result.stdout.decode('utf-8-sig'))

    if current() != target or not check(target, 'initial'):
        raise RuntimeError('Expected valid initial samplepdfreader.pdf; no changes attempted')
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Classes\samplepdfreader.pdf\shell\open\command') as key:
        original_command = winreg.QueryValueEx(key, '')[0]
    control_key = 'Software\\Classes\\' + control
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, control_key):
            raise RuntimeError('Control identity already exists')
    except FileNotFoundError:
        pass
    response = folder / (stamp + '-odbc.rsp')
    response.write_text('REGSVR "' + str(dll) + '"\n', encoding='utf-8', newline='\n')
    plans = {
        'installutil': [framework / 'InstallUtil.exe', '/LogFile=', '/LogToConsole=false', '/U', managed],
        'regsvr32': [windows / 'System32/regsvr32.exe', '/s', dll],
        'odbcconf': [windows / 'System32/odbcconf.exe', '/S', '/F', response],
        'msiexec': [windows / 'System32/msiexec.exe', '/y', dll],
    }
    summary = {'stamp': stamp, 'control_prog_id': control, 'reader_command': original_command,
               'computer_use_used': False, 'pdf_opened': False, 'hosts': [], 'restored': False}
    summary_file = folder / (stamp + '-switch-summary.json')
    created = False
    since = datetime.now().isoformat()
    try:
        for name in options.only:
            host = {'name': name, 'phases': []}
            summary['hosts'].append(host)
            for mode in ['read', 'write']:
                if mode == 'write':
                    if not created:
                        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, control_key) as key:
                            winreg.SetValueEx(key, '', 0, winreg.REG_SZ, 'SamplePdfReader temporary control')
                        created = True
                        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, control_key + r'\shell') as key:
                            winreg.SetValueEx(key, '', 0, winreg.REG_SZ, 'open')
                        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, control_key + r'\shell\open\command') as key:
                            winreg.SetValueEx(key, '', 0, winreg.REG_SZ, original_command)
                    set_via_msbuild(control, name + '-control')
                before = current()
                report = folder / (stamp + '-' + name + '-' + mode + ('.json' if name == 'installutil' else '.log'))
                environment = os.environ.copy()
                environment.update(DFA_HOST_PROBE_MODE=mode, DFA_HOST_PROBE_REPORT=str(report))
                code = invoke(plans[name], name + '-' + mode, environment)
                payload = None
                if report.is_file():
                    text = report.read_text(encoding='utf-8')
                    payload = json.loads(text) if name == 'installutil' else dict(line.split('=', 1) for line in text.splitlines() if '=' in line)
                callback_ok = bool(payload) and (payload.get('write_probe_success') is True or payload.get('probe_success') == 'true')
                after = current()
                independent = check(target, name + '-' + mode + '-independent')
                av = defender(since)
                success = code == 0 and callback_ok and independent and (mode != 'write' or (before == control and after == target))
                phase = {'mode': mode, 'exit_code': code, 'before': before, 'after': after,
                         'success': success, 'payload': payload, 'defender': av}
                host['phases'].append(phase)
                summary_file.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8', newline='\n')
                print(name, mode, 'exit=', code, 'success=', success, 'Defender events=', len(av['Events']), flush=True)
                if av['Events']:
                    raise RuntimeError('Stopped after a Defender event; inspect the local report')
                if not success:
                    if current() != target:
                        set_via_msbuild(target, name + '-failed-restore')
                    break
    finally:
        if current() != target or not check(target, 'final-before-restore'):
            set_via_msbuild(target, 'restore')
        summary['restored'] = current() == target and check(target, 'final')
        if created and summary['restored']:
            # Exact leaves of a uniquely named, previously absent registration only.
            for suffix in [r'\shell\open\command', r'\shell\open', r'\shell', '']:
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, control_key + suffix)
            summary['control_registration_removed'] = True
        summary['final_defender'] = defender(since)
        summary_file.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8', newline='\n')
        print('Report:', summary_file, flush=True)


if __name__ == '__main__':
    main()
