@echo off
rem Authored by AI (Codex). Menu wrapper for the portable VM test script.
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
if not exist "%~dp0test-msbuild-package.ps1" goto missing
:menu
echo.
echo ===== PDF 默认打开方式测试 =====
echo 1. 兼容性预检（不修改设置）
echo 2. 设置包内 SamplePdfReader 为 PDF 默认程序（图标序号 1）
echo 3. 只读复查设置结果
echo 0. 退出
echo.
choice /c 1230 /n /m "请选择 [1/2/3/0]："
if errorlevel 5 exit /b 1
if errorlevel 4 exit /b 0
if errorlevel 3 goto check
if errorlevel 2 goto apply
if errorlevel 1 goto preflight
exit /b 1
:preflight
set "DFA_MENU_MODE=Preflight"
goto run
:apply
set "DFA_MENU_MODE=Set"
goto run
:check
set "DFA_MENU_MODE=Check"
:run
echo.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-msbuild-package.ps1" -Mode %DFA_MENU_MODE%
set "DFA_MENU_EXIT=%errorlevel%"
echo.
if "%DFA_MENU_EXIT%"=="0" echo 本次检查通过。设置后请双击 012.pdf 验证实际打开行为。
if not "%DFA_MENU_EXIT%"=="0" echo 本次操作未通过，请查看上方信息和 results 中的报告。
echo 按任意键返回菜单。
pause >nul
goto menu
:missing
echo 缺少 test-msbuild-package.ps1，请先完整解压测试包。
pause
exit /b 1
