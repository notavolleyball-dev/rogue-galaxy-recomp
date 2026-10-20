@echo off
rem Rogue Galaxy with the profiler ready: each F9 in the game window marks
rem the moment in build\last-run.log and samples every thread for the next
rem 20 seconds into build\run\play-perfil-1.prof, -2, ... (stay where the
rem frame rate drops while it runs). Report afterwards with
rem   python tools\profile\report.py build\run\play-perfil-1.prof build\run\exe-perfil.exe --top 30
rem (exe-perfil.exe is the copy of the build this was recorded with; the
rem report needs the symbols of that same build).
set RG_PROFILE=build\run\play-perfil.prof
set RG_PROFILE_ON_MARK=1
set RG_PROFILE_SECONDS=20
set RG_PROFILE_STACKS=1
rem One [gfx] line a second in build\last-run.log, so a drop can be put to a
rem moment of the session (the line counts from the start of the game).
set DQ8_GFX_STATS_EVERY=60
call "%~dp0RogueGalaxy.bat" %*
