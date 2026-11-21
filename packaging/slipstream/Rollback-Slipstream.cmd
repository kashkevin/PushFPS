@echo off
rem Slipstream test package: back to default behaviour without removing anything. Renames slipstream.ini
rem in every test copy that has one (nothing is deleted); the original build is kept and never reads that
rem file. It does NOT go back to an earlier package: no build is removed or replaced. Package 7's
rem experimental build stays installed beside Package 8's (PushFPS-slipstream-p7), and the stable candidate
rem is the same build in both packages: to use Package 7, start that copy. That copy is a fresh install: the
rem PS5 pairing is shared, but settings made in Package 7's experimental build stayed in PushFPS-slipstream-exp
rem (now the new experimental build). Extra arguments are passed on.
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
