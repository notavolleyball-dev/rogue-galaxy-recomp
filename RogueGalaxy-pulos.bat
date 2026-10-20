@echo off
rem Rogue Galaxy in the 60 fps mode with the jump and position traces: every
rem character's moves off the ground (enemies' leaps included) and every
rem placement go to build\last-run.log, for a boss's leap that misbehaves at
rem 60 fps and not at 30. Mark the moment with F9. A battle's first enemy is
rem at 0x00A36E60; RG_HWWATCH=0x00A36ED0 (its x) with RG_HWWATCH_RANGE set to
rem the floats outside its arena names whatever puts it there.
setlocal
set RG_JUMP_TRACE=3
set RG_POS_TRACE=1
call "%~dp0RogueGalaxy.bat" %*
