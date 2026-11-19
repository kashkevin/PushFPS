@echo off
rem Slipstream test package: if something goes wrong, double-click this file. It reads this PC and the logs of
rem the test copies and makes ONE zip to send, with names, addresses and ids removed; it starts, installs and
rem changes nothing (docs\RELIABILITY.md, section 5). Read the zip before sending it.
setlocal
set "kit=%~dp0testkit\scripts\remoteplay\testkit"
if exist "%kit%\collect-diagnostics.ps1" goto run
echo.
echo   This package is not unpacked yet. Right-click the zip, choose "Extract All",
echo   then start this file from the extracted folder.
echo.
pause
exit /b 2
:run
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%kit%\collect-diagnostics.ps1" %*
set "code=%ERRORLEVEL%"
echo.
pause
exit /b %code%
