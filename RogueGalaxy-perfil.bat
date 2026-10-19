@echo off
rem Rogue Galaxy with the profiler on: samples every thread for fifteen
rem minutes starting one minute after boot (skip the intro, load the save
rem and play, fight, in that window). Report afterwards with
rem   python tools\profile\report.py build\run\play-perfil.prof build\run\exe-perfil.exe --top 30
rem (exe-perfil.exe is the copy of the build this was recorded with; the
rem report needs the symbols of that same build).
set RG_PROFILE=build\run\play-perfil.prof
set RG_PROFILE_START=60
set RG_PROFILE_SECONDS=900
set RG_PROFILE_STACKS=1
rem One [gfx] line a second in build\last-run.log, so a drop can be put to a
rem moment of the session (the line counts from the start of the game).
set DQ8_GFX_STATS_EVERY=60
call "%~dp0RogueGalaxy.bat" %*
