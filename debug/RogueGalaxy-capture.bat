@echo off
rem Rogue Galaxy with F7 capturing a frame, for a picture that looks wrong:
rem press F7 while it is on screen. Every batch the game drew, the GS memory
rem and how the renderer handled each surface go to build\captures
rem (capture-1.txt and .vram, then 2, ...), and the moment to
rem build\last-run.log. A captured frame takes a moment to write.
setlocal
rem The repository's folder, above this one.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
if not exist "%ROOT%\build\captures" mkdir "%ROOT%\build\captures"
set "RG_GS_CAPTURE=%ROOT%\build\captures"
call "%~dp0RogueGalaxy.bat" %*
