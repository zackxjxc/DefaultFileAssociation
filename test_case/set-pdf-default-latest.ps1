# Authored by AI (Codex). Compatibility entry point for the embedded MSBuild backend.
[CmdletBinding()]
param([switch]$CheckOnly, [switch]$OpenPdf)
$testScript = Join-Path $PSScriptRoot 'test-msbuild-package.ps1'
$mode = if ($CheckOnly) { 'Check' } else { 'Set' }
& $testScript -Mode $mode -OpenPdf:$OpenPdf
exit $LASTEXITCODE
