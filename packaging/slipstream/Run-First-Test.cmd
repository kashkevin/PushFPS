@echo off
rem Slipstream test package: a measuring session. Without arguments: the original against the stable candidate
rem (about 35 minutes). Shorter and more useful first:  Run-First-Test.cmd -Session presentation  (about 8 minutes).
rem Double-click it normally (NOT "Run as administrator"). For frame times too, put the PresentMon console
rem program next to this file: Windows then asks once per run to let PresentMon read them.
rem Extra arguments are passed on, for example:  Run-First-Test.cmd -Build experimental  or  -Session versus
setlocal
set "kit=%~dp0testkit\scripts\remoteplay\testkit"
if exist "%kit%\run-first-test.ps1" goto run
echo.
echo   This package is not unpacked yet. Right-click the zip, choose "Extract All",
echo   then start this file from the extracted folder.
echo.
pause
exit /b 2
:run
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%kit%\run-first-test.ps1" -PackageRoot "%~dp0." %*
set "code=%ERRORLEVEL%"
echo.
pause
exit /b %code%
