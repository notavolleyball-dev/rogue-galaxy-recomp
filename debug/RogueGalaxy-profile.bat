@echo off
rem Rogue Galaxy with the profiler ready: each F9 in the game window marks
rem the moment in build\last-run.log and samples every thread for the next
rem 20 seconds into build\run\play-profile-1.prof, -2, ... (numbered on from
rem the files already there; stay where the frame rate drops while it runs).
rem For a hitch over before F9 can be pressed, RogueGalaxy-hitches.bat.
rem Report afterwards with
rem   python tools\profile\report.py build\run\play-profile-1.prof build\run\exe-profile.exe --top 30
rem (exe-profile.exe is the copy of the build this was recorded with; the
rem report needs the symbols of that same build, so it is copied at each
rem start: a report of an older session's profile needs that session's build).
setlocal
rem The repository's folder, above this one.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
if not exist "%ROOT%\build\run" mkdir "%ROOT%\build\run"
if exist "%ROOT%\build\game\src\roguegalaxy.exe" copy /y "%ROOT%\build\game\src\roguegalaxy.exe" "%ROOT%\build\run\exe-profile.exe" >nul
rem Relative paths from here on are the repository's: RogueGalaxy.bat starts
rem the game from there.
set RG_PROFILE=build\run\play-profile.prof
set RG_PROFILE_ON_MARK=1
if not defined RG_PROFILE_SECONDS set RG_PROFILE_SECONDS=20
set RG_PROFILE_STACKS=1
rem One [gfx] line a second in build\last-run.log, so a drop can be put to a
rem moment of the session (the line counts from the start of the game).
if not defined DQ8_GFX_STATS_EVERY set DQ8_GFX_STATS_EVERY=60
rem VU1 microprograms the build lacks (they run on the interpreter, several
rem times slower) are recorded into build\vu1-profile, which the next build
rem compiles in.
set PS2_VU_PROGRAM_PROFILE=build\vu1-profile
call "%~dp0RogueGalaxy.bat" %*
