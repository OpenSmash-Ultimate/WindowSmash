@echo off
setlocal
set "SSBU_DATA_ROOT="
set "SSBU_INPUT="
set "SSBU_NET_HOST="
set "SSBU_NET_RELAY="
set "SSBU_NET_BIND="
@echo off
rem Starts the game as the netplay host: it runs the relay the other players join (UDP port
rem 39420, opened on the router with UPnP when the router allows it). See docs\MULTIPLAYER.md.
rem The address to give the other players appears in the game's overlay (F2) and in
rem netplay_address.txt; the log goes to build\play.log. Every player needs game version 13.0.5.
cd /d "%~dp0.."
setlocal
if not exist build\ssbu.exe (echo The game isn't built here yet: run Install WindowSmash.bat. & pause & exit /b 1)
echo Every player needs game version 13.0.5, as you have.
set "SSBU_NET_NAME=%USERNAME%"
set /p "SSBU_NET_NAME=Your player name [%SSBU_NET_NAME%]: "
set "SSBU_NET_ROOM=smash"
set /p "SSBU_NET_ROOM=Room name - the other players use the same one [smash]: "
set "SSBU_NET_PORT=39420"
set /p "SSBU_NET_PORT=UDP port [39420]: "
set "SSBU_NET_HOST=1"
if exist netplay_address.txt del netplay_address.txt
start "SSBU netplay host log" /min cmd /c "build\ssbu.exe dump\modules --status 30 > build\play.log 2>&1"
echo.
echo Starting the game as host of room "%SSBU_NET_ROOM%"...
echo In the game: Local Wireless ^> Create a Room. The others pick your room from the list.
echo.
for /l %%i in (1,1,20) do (
    if not exist netplay_address.txt timeout /t 1 /nobreak > nul
)
if exist netplay_address.txt (type netplay_address.txt) else echo (The address to share will show in the game's overlay - press F2.)
echo.
pause
