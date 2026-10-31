# Authored by AI (Codex). Portable PDF test; requires Windows PowerShell 5.1 or later.
# Set writes only when requested; Preflight and Check do not register or set defaults.
[CmdletBinding()]
param([ValidateSet('Preflight','Set','Check')][string]$Mode = 'Set', [switch]$OpenPdf)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) { $PSNativeCommandUseErrorActionPreference = $false }
$tool = Join-Path $PSScriptRoot 'DefaultFileAssociation.exe'
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
    $tool = Join-Path (Split-Path -Parent $PSScriptRoot) 'out\build\windows-x86-v143\Release\DefaultFileAssociation.exe'
}
$reader = Join-Path $PSScriptRoot 'SamplePdfReader.exe'
$pdf = Join-Path $PSScriptRoot '012.pdf'
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw 'DefaultFileAssociation.exe is missing.' }
if ($Mode -eq 'Set' -and -not (Test-Path -LiteralPath $reader -PathType Leaf)) { throw 'SamplePdfReader.exe is missing.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if ([Security.Principal.WindowsPrincipal]::new($identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run as a normal user, not as administrator.' }
$runDirectory = Join-Path (Join-Path $PSScriptRoot 'results') ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $runDirectory)
$utf8 = [Text.UTF8Encoding]::new($false)
function Read-Value {
    param([Microsoft.Win32.RegistryKey]$Hive, [string]$Path, [string]$Name)
    $key = $Hive.OpenSubKey($Path, $false)
    if ($null -eq $key) { return $null }
    try { return $key.GetValue($Name, $null) } finally { $key.Dispose() }
}
function Snapshot {
    $latest = 'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoiceLatest'
    $classes = 'Software\Classes\samplepdfreader.pdf'
    return [ordered]@{
        LatestProgId = Read-Value ([Microsoft.Win32.Registry]::CurrentUser) ($latest + '\ProgId') 'ProgId'
        LatestHash = Read-Value ([Microsoft.Win32.Registry]::CurrentUser) $latest 'Hash'
        RegisteredCommand = Read-Value ([Microsoft.Win32.Registry]::CurrentUser) ($classes + '\shell\open\command') ''
        DefaultIcon = Read-Value ([Microsoft.Win32.Registry]::CurrentUser) ($classes + '\DefaultIcon') ''
        ApplicationIcon = Read-Value ([Microsoft.Win32.Registry]::CurrentUser) ($classes + '\Application') 'ApplicationIcon'
    }
}
$before = Snapshot
$started = Get-Date
$toolArguments = @('--msbuild','--ext=.pdf','--prog-id=samplepdfreader.pdf', ('--log-file=' + (Join-Path $runDirectory 'tool.log')))
if ($Mode -eq 'Preflight') { $toolArguments += '--preflight' }
elseif ($Mode -eq 'Check') { $toolArguments += '--check' }
else { $toolArguments += @(('--exe-path=' + $reader), '--icon-index=1') }
$savedPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    $LASTEXITCODE = $null
    & $tool @toolArguments *> (Join-Path $runDirectory 'console.log')
    $toolExit = if ($null -eq $LASTEXITCODE) { 9009 } else { [int]$LASTEXITCODE }
} finally { $ErrorActionPreference = $savedPreference }
$after = Snapshot
$log = ''
if (Test-Path -LiteralPath (Join-Path $runDirectory 'tool.log')) { $log = [IO.File]::ReadAllText((Join-Path $runDirectory 'tool.log'), $utf8) }
$taskReports = @([regex]::Matches($log, '(?m)^\{[^\r\n]*"completed_registry_writes"[^\r\n]*\}\s*$') | ForEach-Object { $_.Value | ConvertFrom-Json })
$expectedCommand = '"' + $reader + '" "%1" %*'
$expectedIcon = '"' + $reader + '",1'
$success = $toolExit -eq 0 -and $taskReports.Count -gt 0
foreach ($taskReport in $taskReports) { $success = $success -and $taskReport.success }
if ($Mode -eq 'Set' -and $taskReports.Count -gt 0) { $success = $success -and $taskReports[-1].completed_registry_writes -eq 2 }
if ($Mode -ne 'Set') {
    $success = $success -and ($before | ConvertTo-Json -Compress) -ceq ($after | ConvertTo-Json -Compress)
    foreach ($taskReport in $taskReports) { $success = $success -and $taskReport.completed_registry_writes -eq 0 }
}
if ($Mode -ne 'Preflight') {
    $success = $success -and $after.LatestProgId -ieq 'samplepdfreader.pdf' -and
        $after.RegisteredCommand -ieq $expectedCommand -and $after.DefaultIcon -ieq $expectedIcon -and
        $after.ApplicationIcon -ieq $expectedIcon -and $log -match '(?m)^shell_matches=true\r?$'
}
$avEvents = @()
$avReadError = $null
try {
    $avEvents = @(Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-Windows Defender/Operational';Id=1116,1117;StartTime=$started} -ErrorAction Stop | Select-Object TimeCreated,Id,Message)
} catch { $avReadError = $_.Exception.Message }
$report = [ordered]@{
    Timestamp = [DateTimeOffset]::Now.ToString('o'); Mode=$Mode; ToolExitCode=$toolExit
    Tool=$tool; Reader=$reader; ProgId='samplepdfreader.pdf'; IconIndex=1
    Before=$before; After=$after; TaskReports=$taskReports; AssociationSuccess=[bool]$success
    DefenderEvents=$avEvents; DefenderQueryError=$avReadError
    Note='AssociationSuccess reports association verification; review Defender events separately. Preflight success only confirms compatibility.'
}
[IO.File]::WriteAllText((Join-Path $runDirectory 'report.json'), ($report | ConvertTo-Json -Depth 8), $utf8)
Write-Host ('Report directory: ' + $runDirectory)
Write-Host ('Mode=' + $Mode + '; ToolExitCode=' + $toolExit + '; AssociationSuccess=' + $success)
if (-not $success) { exit 1 }
if ($OpenPdf) {
    if ($Mode -eq 'Preflight') { throw '-OpenPdf is not available with Preflight.' }
    if (-not (Test-Path -LiteralPath $pdf -PathType Leaf)) { throw '012.pdf is missing.' }
    Start-Process -FilePath $pdf -WorkingDirectory $PSScriptRoot
}
exit 0
