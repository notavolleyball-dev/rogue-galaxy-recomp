@echo off
rem Rogue Galaxy at the game's own 30 frames per second (RogueGalaxy.bat
rem plays in the 60 fps mode): to tell whether a problem comes from the 60
rem fps mode.
setlocal
set RG_FPS=30
call "%~dp0RogueGalaxy.bat" %*
