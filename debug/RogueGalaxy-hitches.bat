@echo off
rem A short session for hitches: the profile launcher, with the stats line
rem every 10 frames instead of every 60, so a drop of a few frames shows in
rem build\last-run.log next to the F9 mark pressed when it is felt. Each line
rem carries the longest frame of its 10, the longest the main thread was held
rem up, new GPU pipelines and what creating them cost, and the VU1 pairs that
rem ran on the interpreter. The profiler samples all the time (which costs
rem the VIF1 thread about 9%, so play with Rogue Galaxy.exe otherwise): a
rem hitch nobody marked (a frame of 50 ms or more, or the frame rate halved
rem for a few frames) writes a profile by itself, of the hitch and half a
rem second either side, up to 30 a session, and F9 marks and writes the 10
rem seconds before the press and the 5 after. The log grows some 10 KB a
rem second: a few minutes is plenty.
setlocal
set DQ8_GFX_STATS_EVERY=10
set RG_PROFILE_BEFORE=10
set RG_PROFILE_SECONDS=5
set RG_PROFILE_AUTO_MS=50
call "%~dp0RogueGalaxy-profile.bat" %*
