@echo off
rem PushFPS diagnostics: double-click this file. It reads this PC and makes ONE zip to send; it starts,
rem installs and changes nothing. What is collected and what is not: see the top of collect-diagnostics.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect-diagnostics.ps1" %*
echo.
pause
