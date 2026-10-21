@echo off
setlocal
set "SSBU_DATA_ROOT="
set "SSBU_INPUT="
set "SSBU_NET_HOST="
set "SSBU_NET_RELAY="
set "SSBU_NET_BIND="
set "SSBU_NET_ROOM="
set "SSBU_NET_NAME="
@echo off
rem Starts the native Super Smash Bros. Ultimate offline (Play SSBU.bat plays online). Its log goes
rem to build\play.log.
cd /d "%~dp0.."
if not exist build\ssbu.exe (echo The game isn't built here yet: run Install WindowSmash.bat. & pause & exit /b 1)
start "SSBU log" /min cmd /c "build\ssbu.exe dump\modules --status 30 > build\play.log 2>&1"
