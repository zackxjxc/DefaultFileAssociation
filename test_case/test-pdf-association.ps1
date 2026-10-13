# Authored by AI (Codex). Sets the current user's real .pdf default association.
# Run from a normal, non-administrator PowerShell session.
# Use -CheckOnly to verify the existing association without setting it again.
# Requires the Release build at out/build/windows-x86-v143/Release.
[CmdletBinding()]
param([switch]$CheckOnly)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$associationTool = Join-Path $projectRoot 'out\build\windows-x86-v143\Release\DefaultFileAssociation.exe'
$readerExecutable = Join-Path $PSScriptRoot 'DocBox.exe'
$extension = '.pdf'
$progId = 'docboxtest.pdf'

foreach ($requiredFile in @($associationTool, $readerExecutable)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required executable is missing: $requiredFile"
    }
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from a normal, non-administrator PowerShell session.'
}

$runName = 'pdf-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$resultsRoot = Join-Path $PSScriptRoot 'results'
$runDirectory = Join-Path $resultsRoot $runName
[void](New-Item -ItemType Directory -Path $runDirectory)
$utf8 = [Text.UTF8Encoding]::new($false)

function Read-UserRegistryString {
    param([string]$SubKey, [string]$ValueName = '')
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($SubKey, $false)
    if ($null -eq $key) { return $null }
    try { return $key.GetValue($ValueName, $null) }
    finally { $key.Dispose() }
}

function Get-AssociationState {
    $choiceKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoice'
    [pscustomobject][ordered]@{
        UserChoiceProgId = Read-UserRegistryString $choiceKey 'ProgId'
        UserChoiceHash = Read-UserRegistryString $choiceKey 'Hash'
        ClassesProgId = Read-UserRegistryString 'Software\Classes\.pdf'
        ExplorerProgId = Read-UserRegistryString 'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf' 'ProgId'
        RegisteredOpenCommand = Read-UserRegistryString 'Software\Classes\docboxtest.pdf\shell\open\command'
    }
}

function Invoke-AssociationTool {
    param([string]$Stage, [string[]]$ToolArguments)
    $stageLog = Join-Path $runDirectory ($Stage + '.log')
    Write-Host "`n[$Stage]"
    # The tool writes UTF-8 diagnostics itself; retain the real native exit code.
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $associationTool @ToolArguments "--log-file=$stageLog" | Out-Host
        $stageExit = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $savedPreference }
    Write-Host "$Stage exit code: $stageExit"
    return [int]$stageExit
}

$before = Get-AssociationState
$beforePath = Join-Path $runDirectory 'before.json'
[IO.File]::WriteAllText($beforePath, ($before | ConvertTo-Json -Depth 4), $utf8)
Write-Host "Reader: $readerExecutable"
Write-Host "Target: $extension -> $progId"
Write-Host "Previous UserChoice: $($before.UserChoiceProgId)"
Write-Host "Results: $runDirectory"

$setExit = $null
if (-not $CheckOnly) {
    $setArguments = @("--ext=$extension", "--exe-path=$readerExecutable", "--prog-id=$progId", '--description=DocBox Test PDF')
    $setExit = Invoke-AssociationTool 'set' $setArguments
    # Give Explorer time to receive the change notification before querying again.
    Start-Sleep -Seconds 2
}

$checkArguments = @('--check', "--ext=$extension", "--prog-id=$progId")
$checkExit = Invoke-AssociationTool 'check' $checkArguments
$diagnoseExit = Invoke-AssociationTool 'diagnose' @('--diagnose', "--ext=$extension", "--prog-id=$progId")
$after = Get-AssociationState
$diagnosticText = [IO.File]::ReadAllText((Join-Path $runDirectory 'diagnose.log'), $utf8)
$shellProgIdMatch = [regex]::Match($diagnosticText, '(?m)^Shell\.effective_progid: hr=0x00000000, value=(.+)\r?$')
$shellCommandMatch = [regex]::Match($diagnosticText, '(?m)^Shell\.open_command: hr=0x00000000, value=(.+)\r?$')
$effectiveProgId = $null
$effectiveCommand = $null
if ($shellProgIdMatch.Success) { $effectiveProgId = $shellProgIdMatch.Groups[1].Value.TrimEnd([char]13) }
if ($shellCommandMatch.Success) { $effectiveCommand = $shellCommandMatch.Groups[1].Value.TrimEnd([char]13) }
$expectedCommand = '"' + $readerExecutable + '" "%1" %*'
$registryMatches = $after.UserChoiceProgId -ieq $progId -and
    $after.ClassesProgId -ieq $progId -and $after.ExplorerProgId -ieq $progId -and
    $after.RegisteredOpenCommand -ieq $expectedCommand
# --check verifies the UserChoice hash and the effective Windows Shell ProgID/command.
# Merely registering a handler or collecting diagnostics is not a successful test.
$shellMatches = $effectiveProgId -ieq $progId -and $effectiveCommand -ieq $expectedCommand
$success = ($CheckOnly -or $setExit -eq 0) -and $checkExit -eq 0 -and
    $diagnoseExit -eq 0 -and $registryMatches -and $shellMatches
$report = [pscustomobject][ordered]@{
    Timestamp = [DateTimeOffset]::Now.ToString('o')
    CheckOnly = [bool]$CheckOnly
    ReaderExecutable = $readerExecutable
    Extension = $extension
    ExpectedProgId = $progId
    SetExitCode = $setExit
    CheckExitCode = $checkExit
    DiagnoseExitCode = $diagnoseExit
    RegistryMatches = [bool]$registryMatches
    EffectiveShellProgId = $effectiveProgId
    EffectiveShellOpenCommand = $effectiveCommand
    ShellMatches = [bool]$shellMatches
    Success = [bool]$success
    Before = $before
    After = $after
}
$reportPath = Join-Path $runDirectory 'report.json'
[IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 4), $utf8)
Write-Host "`nReport: $reportPath"
if ($success) {
    Write-Host 'PASS: UserChoice, hash, registered command and effective Windows Shell association verified.'
    exit 0
}
Write-Host 'FAIL: The requested PDF default did not pass all checks. See report.json and stage logs.'
exit 1
