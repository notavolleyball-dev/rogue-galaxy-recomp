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
rem 60 fps mode (experimental, see RG_FPS in README.md): remove the line for 30.
set RG_FPS=60
rem Keeps other programs' Vulkan layers (OBS's capture hook, ReShade, the
rem Steam, GOG and Epic overlays) out of the game: they load into every
rem Vulkan process and sit on its presents. Remove the line to allow them.
set VK_LOADER_LAYERS_DISABLE=~implicit~

echo Rogue Galaxy is running. Close the game window to quit.
"%GAME%" "disc\SCUS_974.90" %* > "build\last-run.log" 2>&1
rem Not "if errorlevel 1": a crash exits with a negative code (0xC0000005).
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo The game stopped with an error ^(exit code %RC%^). The end of build\last-run.log:
    powershell -NoProfile -Command "Get-Content -Tail 15 'build\last-run.log'"
    pause
)
exit /b %RC%
