@echo off
rem Compiles the game's shaders ahead of time (tools\shader_aot.py): an effect you have not seen
rem yet then shows at once the first time, instead of a moment later. It runs once, from minutes
rem to about two hours. It can be stopped (close this window) and started again: what is done
rem stays done.
rem Needs Python and the build tools, as for building (docs\BUILDING.md).
cd /d "%~dp0.."
setlocal
call tools\find_python.bat || (pause & exit /b 1)
if not exist build\ssbu.exe (echo The game isn't built here yet: run Install WindowSmash.bat. & pause & exit /b 1)

echo.
echo  How fast should the shaders be compiled?
echo.
echo   [1] Quiet    - low priority, cores minus 2 (the PC stays usable)   [default]
echo   [2] Balanced - normal priority, all cores (the PC may lag a bit)
echo   [3] Fast     - HIGH priority, all cores (the PC will be slow until it finishes)
echo   [4] Custom   - you choose the priority and the number of processes
echo.
choice /c 1234 /n /t 20 /d 1 /m " Choose 1-4 (1 in 20 seconds): "
set "OPTS="
if errorlevel 4 goto custom
if errorlevel 3 set "OPTS=--priority high --jobs 0" & goto run
if errorlevel 2 set "OPTS=--priority normal --jobs 0" & goto run
goto run

:custom
echo.
set "P=low"
set /p "P= Priority (low / normal / high) [low]: "
set "J=0"
set /p "J= Processes (0 = one per core) [0]: "
set "OPTS=--priority %P% --jobs %J%"

:run
echo.
echo  Starting: shader_aot.py %OPTS%
echo.
%PY% tools\shader_aot.py %OPTS%
pause
