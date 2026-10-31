@echo off
setlocal
set "SSBU_DATA_ROOT="
set "SSBU_INPUT="
rem [rollback research build: build\ssbu_rollback.exe - see patches\rollback\README.md]
rem Offline, like Play SSBU Offline (nvfix).bat. Its log goes to build\play_rollback.log.
set "SSBU_RB=1"
cd /d "%~dp0.."
if not exist build\ssbu_rollback.exe (echo build\ssbu_rollback.exe isn't built: run patches\rollback\build_rollback.bat. & pause & exit /b 1)
start "SSBU log" /min cmd /c "build\ssbu_rollback.exe dump\modules --status 30 > build\play_rollback.log 2>&1"
