@echo off
setlocal
cd /d "%~dp0.."
if not exist build\ssbu.exe (echo The game isn't built here yet: run Install WindowSmash.bat. & pause & exit /b 1)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\Unlock-All-Fighters.ps1" %*
set "SSBU_UNLOCK_RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %SSBU_UNLOCK_RESULT%
