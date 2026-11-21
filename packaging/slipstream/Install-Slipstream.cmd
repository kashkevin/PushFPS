@echo off
rem Slipstream test package: installs the builds in this package side by side (original, stable
rem candidate, experimental, and Package 7's experimental build as the rollback copy) with each build's
rem own installer. Safe to run again. Extra arguments are passed on, for example:
rem   Install-Slipstream.cmd -Only stable -Force
rem   Install-Slipstream.cmd -NeuralRuntime Yes     (or No; default Ask: the one question it asks. The
rem                                                optional NVIDIA neural runtime is kept only on yes, and
rem                                                only when it is an NVIDIA-signed original.)
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
