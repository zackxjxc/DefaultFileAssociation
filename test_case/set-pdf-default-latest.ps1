# Authored by AI (Codex). Build-specific Windows 11 PDF association experiment.
# Uses Windows .NET Framework MSBuild and the analyzed UserChoiceLatest routine.
# Default: set and verify. -CheckOnly reads state. -OpenPdf opens 012.pdf on success.
[CmdletBinding()]
param([switch]$CheckOnly, [switch]$OpenPdf)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$projectRoot = Split-Path -Parent $PSScriptRoot
$reader = Join-Path $PSScriptRoot 'DocBox.exe'
$pdf = Join-Path $PSScriptRoot '012.pdf'
$tool = Join-Path $projectRoot 'out\build\windows-x86-v143\Release\DefaultFileAssociation.exe'
$probe = Join-Path $projectRoot 'research\scripts\msbuild-latest-probe.proj'
$msbuild = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\MSBuild.exe'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if ([Security.Principal.WindowsPrincipal]::new($identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run from a normal, non-administrator PowerShell session.'
}
foreach ($file in @($reader, $tool, $probe, $msbuild)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required file missing: $file" }
}
if ($OpenPdf -and -not (Test-Path -LiteralPath $pdf -PathType Leaf)) { throw "Test PDF missing: $pdf" }

$runName = 'latest-pdf-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$runDirectory = Join-Path (Join-Path $PSScriptRoot 'results') $runName
[void](New-Item -ItemType Directory -Path $runDirectory)
$utf8 = [Text.UTF8Encoding]::new($false)
$expectedCommand = '"' + $reader + '" "%1" %*'

function Get-RegisteredCommand {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Classes\docboxtest.pdf\shell\open\command', $false)
    if ($null -eq $key) { return $null }
    try { return $key.GetValue('', $null) } finally { $key.Dispose() }
}
function Invoke-LoggedNative {
    param([string]$Executable, [string[]]$CommandArguments, [string]$ConsoleLog)
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @CommandArguments *> $ConsoleLog
        return [int]$LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
}

$registrationExit = $null
if ((Get-RegisteredCommand) -ine $expectedCommand) {
    if ($CheckOnly) { throw 'The requested DocBox handler has not been registered.' }
    # The legacy tool registers the handler before its old UserChoice path runs.
    # Its exit code is retained; only the independently verified final state passes.
    $registrationLog = Join-Path $runDirectory 'registration.log'
    $registrationExit = Invoke-LoggedNative $tool @('--ext=.pdf', "--exe-path=$reader", '--prog-id=docboxtest.pdf', "--log-file=$registrationLog") (Join-Path $runDirectory 'registration-console.log')
    if ((Get-RegisteredCommand) -ine $expectedCommand) { throw "Handler registration failed; see $runDirectory" }
}

$nativeReportPath = Join-Path $runDirectory 'msbuild-report.json'
$attempt = if ($CheckOnly) { 'false' } else { 'true' }
$msbuildExit = Invoke-LoggedNative $msbuild @($probe, '/ToolsVersion:4.0', "/p:AttemptSet=$attempt", "/p:ReportFile=$nativeReportPath", '/nologo', '/v:minimal') (Join-Path $runDirectory 'msbuild-console.log')
$nativeReport = $null
if (Test-Path -LiteralPath $nativeReportPath) {
    $nativeReport = [IO.File]::ReadAllText($nativeReportPath, $utf8) | ConvertFrom-Json
}
if (-not $CheckOnly) { Start-Sleep -Seconds 2 }
$shellLog = Join-Path $runDirectory 'shell.log'
$diagnoseExit = Invoke-LoggedNative $tool @('--diagnose', '--ext=.pdf', '--prog-id=docboxtest.pdf', "--log-file=$shellLog") (Join-Path $runDirectory 'shell-console.log')
$effectiveProgId = $null
$effectiveCommand = $null
if (Test-Path -LiteralPath $shellLog) {
    $text = [IO.File]::ReadAllText($shellLog, $utf8)
    $idMatch = [regex]::Match($text, '(?m)^Shell\.effective_progid: hr=0x00000000, value=(.+)\r?$')
    $commandMatch = [regex]::Match($text, '(?m)^Shell\.open_command: hr=0x00000000, value=(.+)\r?$')
    if ($idMatch.Success) { $effectiveProgId = $idMatch.Groups[1].Value.TrimEnd([char]13) }
    if ($commandMatch.Success) { $effectiveCommand = $commandMatch.Groups[1].Value.TrimEnd([char]13) }
}
$success = $msbuildExit -eq 0 -and $diagnoseExit -eq 0 -and $null -ne $nativeReport -and
    $nativeReport.write_probe_success -and $nativeReport.after_prog_id -ieq 'docboxtest.pdf' -and
    $effectiveProgId -ieq 'docboxtest.pdf' -and $effectiveCommand -ieq $expectedCommand -and
    (Get-RegisteredCommand) -ieq $expectedCommand
$report = [pscustomobject][ordered]@{
    Timestamp = [DateTimeOffset]::Now.ToString('o')
    CheckOnly = [bool]$CheckOnly
    ReaderExecutable = $reader
    ProgId = 'docboxtest.pdf'
    RegistrationExitCode = $registrationExit
    MsbuildExitCode = $msbuildExit
    MsbuildExecutable = $msbuild
    DiagnoseExitCode = $diagnoseExit
    EffectiveShellProgId = $effectiveProgId
    EffectiveShellOpenCommand = $effectiveCommand
    NativeResult = $nativeReport
    Success = [bool]$success
    Limitation = 'Uses Windows .NET Framework MSBuild; validated for this exact windows.storage.dll SHA-256. Other DLL builds are rejected.'
}
$reportPath = Join-Path $runDirectory 'report.json'
[IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 6), $utf8)
Write-Host "Report: $reportPath"
if (-not $success) {
    Write-Host "FAIL: MSBuild=$msbuildExit; diagnosis=$diagnoseExit; effective ProgID=$effectiveProgId"
    exit 1
}
Write-Host "PASS: .pdf -> docboxtest.pdf -> $reader; latest hash and effective Shell command verified."
if ($OpenPdf) {
    $viewer = Start-Process -FilePath $pdf -WorkingDirectory $PSScriptRoot -PassThru
    if ($null -ne $viewer) { Write-Host "PDF viewer PID: $($viewer.Id)" }
}
exit 0
