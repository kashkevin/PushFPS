@echo off
rem Slipstream test package: the full session (about 75 minutes): the quick latency arms, a 10-minute
rem stability run and the reliability checks, with a break before each part. Run-First-Test.cmd first.
rem Double-click it normally (NOT "Run as administrator"). For frame times too, put the PresentMon console
rem program next to this file: Windows then asks once per run to let PresentMon read them.
rem Extra arguments are passed on, for example:  Run-Full-Test.cmd -Build experimental
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
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%kit%\run-first-test.ps1" -Session full %*
set "code=%ERRORLEVEL%"
echo.
pause
exit /b %code%
