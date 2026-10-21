@echo off
rem Rogue Galaxy in the 60 fps mode without the AI step fix (src/step_fixes.cpp,
rem "ai"): to tell whether a problem with how characters move comes from it.
setlocal
set RG_STEP_FIXES=clock,movie,npc,judge,gravity,event,menu
call "%~dp0RogueGalaxy.bat" %*
