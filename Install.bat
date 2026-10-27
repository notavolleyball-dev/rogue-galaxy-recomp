@echo off
rem Rogue Galaxy Recompiled: builds the game on this PC from your own disc
rem image. Double-click it, or drop the .iso onto it. The work is done by
rem tools\install\install.ps1, which says more.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install\install.ps1" %*
echo.
pause
