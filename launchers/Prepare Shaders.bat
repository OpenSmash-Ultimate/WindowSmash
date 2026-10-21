@echo off
rem Compiles the game's shaders ahead of time (tools\shader_aot.py): an effect you have not seen
rem yet then shows at once the first time, instead of a moment later. It runs once, from minutes
rem to about two hours, at low priority (the PC stays usable). It can be stopped (close this
rem window) and started again: what is done stays done.
rem Needs Python and the build tools, as for building (docs\BUILDING.md).
cd /d "%~dp0.."
setlocal
call tools\find_python.bat || (pause & exit /b 1)
if not exist build\ssbu.exe (echo The game isn't built here yet: run Install WindowSmash.bat. & pause & exit /b 1)
%PY% tools\shader_aot.py
pause
