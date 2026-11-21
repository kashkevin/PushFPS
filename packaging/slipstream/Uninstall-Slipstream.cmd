@echo off
rem Slipstream test package: removes the three test copies (PushFPS-original, PushFPS-slipstream,
rem PushFPS-slipstream-exp under %LOCALAPPDATA%\Programs) after you type YES. Never touches an official
rem PushFPS install, its shortcuts or settings, or the PS5 pairing. Extra arguments are passed on.
setlocal
set "kit=%~dp0testkit\scripts\remoteplay\testkit"
if exist "%kit%\uninstall-slipstream.ps1" goto run
echo.
echo   This package is not unpacked yet. Right-click the zip, choose "Extract All",
echo   then start this file from the extracted folder.
echo.
pause
exit /b 2
:run
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%kit%\uninstall-slipstream.ps1" -Mode All %*
set "code=%ERRORLEVEL%"
echo.
pause
exit /b %code%
