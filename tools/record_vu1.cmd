@echo off
rem Plays Midnight Club II on the PC and records every VU1 program the Switch build has no compiled version of.
rem Play as much as you can (all cities, cars, modes, split-screen, replays); close the window when done.
rem New recordings land in MC2Recomp\ps2-native-recomp\games\mc2\vu1_new; tell Claude to regenerate afterwards.
setlocal
cd /d "%~dp0MC2Recomp\ps2-native-recomp"
if not exist games\mc2\vu1_new mkdir games\mc2\vu1_new
set PS2_VU_RECORD=%CD%\games\mc2\vu1_new
set PS2_LIVE=0
build\native-mc2\ps2run.exe --disc games\mc2\disc.iso --game-dir games\mc2 --memcard games\mc2\pc_record.ps2 --window
echo.
echo Recorded programs:
dir /b games\mc2\vu1_new
pause
