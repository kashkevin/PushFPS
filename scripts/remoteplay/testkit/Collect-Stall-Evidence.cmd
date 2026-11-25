@echo off
rem PushFPS capture-stall evidence: double-click this file AFTER playing. READ-ONLY: it changes nothing on this PC, needs no
rem administrator rights, sends nothing anywhere and writes ONE text file into PushFPS's logs folder, which stays on this PC
rem unless you send it. Add "-Mode watch" to watch the next quarter hour while you play (light; it measures and limits its
rem own cost). What it reads: see the top of Collect-Stall-Evidence.ps1 and of its output.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-Stall-Evidence.ps1" %*
echo.
pause
