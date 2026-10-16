@echo off
rem Starts Rogue Galaxy Recompiled on the extracted disc in disc\.
rem Double-click it, or run it from a terminal; extra arguments go to the game.
rem The game's log is written to build\last-run.log.
setlocal
cd /d "%~dp0"

set "GAME=build\game\src\roguegalaxy.exe"
if not exist "%GAME%" (
    echo The game is not built yet: see "Build" in README.md.
    pause
    exit /b 1
)
if not exist "disc\SCUS_974.90" (
    echo No extracted disc in disc\: run  python setup.py extract  first.
    pause
    exit /b 1
)

rem Options (see "Running" in README.md), for example:
rem   set RG_SHOW_FPS=1
rem   set RG_MUTE=1

echo Rogue Galaxy is running. Close the game window to quit.
"%GAME%" "disc\SCUS_974.90" %* > "build\last-run.log" 2>&1
if errorlevel 1 (
    echo The game stopped with an error. The end of build\last-run.log:
    powershell -NoProfile -Command "Get-Content -Tail 15 'build\last-run.log'"
    pause
)
