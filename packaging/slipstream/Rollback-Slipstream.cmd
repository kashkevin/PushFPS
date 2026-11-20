@echo off
rem Slipstream test package: back to default behaviour without removing anything. Renames slipstream.ini
rem in the stable candidate and the experimental build (nothing is deleted); the original build is kept
rem and never reads that file. Extra arguments are passed on.
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
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%kit%\uninstall-slipstream.ps1" -Mode Settings %*
set "code=%ERRORLEVEL%"
echo.
pause
exit /b %code%
