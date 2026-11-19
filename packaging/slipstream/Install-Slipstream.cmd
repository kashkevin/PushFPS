@echo off
rem Slipstream test package: installs the builds in this package side by side (original, stable
rem candidate, experimental) with each build's own installer. Safe to run again. Extra arguments are
rem passed on, for example:  Install-Slipstream.cmd -Only stable -Force
setlocal
set "kit=%~dp0testkit\scripts\remoteplay\testkit"
if exist "%kit%\install-slipstream.ps1" goto run
echo.
echo   This package is not unpacked yet. Right-click the zip, choose "Extract All",
echo   then start this file from the extracted folder.
echo.
pause
exit /b 2
:run
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%kit%\install-slipstream.ps1" -PackageRoot "%~dp0." %*
set "code=%ERRORLEVEL%"
echo.
pause
exit /b %code%
