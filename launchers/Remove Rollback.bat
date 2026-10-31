@echo off
setlocal
rem SMASH BROS\launchers\Remove Rollback.bat: takes multiplayer (rollback netplay) out of the folder it sits in: the
rem MULTIPLAYER shortcut, the rollback build and its build folders, its logs, its launchers (this file last). Your game
rem files, your save, the normal game and WindowSmash's own launchers stay; so does the folder's name.
for %%r in ("%~dp0..") do set "WS=%%~fr"
if not exist "%WS%\patches\rollback" if not exist "%WS%\build\ssbu_rollback.exe" (echo No multiplayer files in "%WS%". & pause & exit /b 0)
tasklist /fi "imagename eq ssbu_rollback.exe" | find /i "ssbu_rollback.exe" >nul && (echo The game is running: close it first. & pause & exit /b 1)
echo This removes SMASH BROS multiplayer (rollback netplay) from "%WS%":
echo   the MULTIPLAYER shortcut, build\ssbu_rollback.exe and its build folders (about 1 GB), its logs,
echo   the rollback launchers, controllers_rollback_p1.ini and patches\rollback\
echo Your game files, save and the normal game stay.
echo.
set "OK="
set /p "OK=Remove it? (Y/N): "
if /i not "%OK%"=="Y" (echo Nothing removed. & pause & exit /b 0)
del /q "%WS%\MULTIPLAYER.lnk" 2>nul
del /q "%WS%\build\ssbu_rollback.exe" "%WS%\build\ssbu_rollback.map" "%WS%\build\rollback_update.log" 2>nul
del /q "%WS%\build\play_rollback_net_*.log" "%WS%\build\rb_net_*.txt" "%WS%\build\rollback_address.txt" "%WS%\build\rollback_last_host.txt" 2>nul
for %%d in (main_rb rt_rollback rb_root rollback_update) do if exist "%WS%\build\%%d" rmdir /s /q "%WS%\build\%%d"
del /q "%WS%\launchers\Rollback Netplay.bat" "%WS%\launchers\rollback_netplay.ps1" "%WS%\launchers\Rollback Netplay (command prompt).bat" "%WS%\launchers\Play SSBU Offline (rollback).bat" "%WS%\launchers\smash_multiplayer.ps1" "%WS%\launchers\multiplayer.ico" 2>nul
del /q "%WS%\controllers_rollback_p1.ini" 2>nul
if exist "%WS%\patches\rollback" rmdir /s /q "%WS%\patches\rollback"
if exist "%LOCALAPPDATA%\SMASH BROS Multiplayer" rmdir /s /q "%LOCALAPPDATA%\SMASH BROS Multiplayer"
echo Removed. (SDL3.dll stays: WindowSmash itself uses it.)
pause
rem (this file last: a batch file can't keep reading itself once deleted)
(goto) 2>nul & del "%~f0"
