@echo off
setlocal
set "SSBU_DATA_ROOT="
set "SSBU_INPUT="
set "SSBU_NET_HOST="
set "SSBU_NET_RELAY="
set "SSBU_NET_BIND="
@echo off
rem Starts the game as a netplay player joining a host's relay (or a standalone relay,
rem tools\ssbu_relay.py). No port forwarding needed here. See docs\MULTIPLAYER.md. The log goes to
rem build\play.log. The host needs game version 13.0.5 too.
cd /d "%~dp0.."
setlocal
if not exist build\ssbu.exe (echo The game isn't built here yet: run Install WindowSmash.bat. & pause & exit /b 1)
echo The host needs game version 13.0.5 too.
set "SSBU_NET_RELAY="
set /p "SSBU_NET_RELAY=Host's address (IP or IP:port, from the host's netplay_address.txt): "
if "%SSBU_NET_RELAY%"=="" (
    echo An address is needed - ask the host for theirs.
    pause
    exit /b 1
)
set "SSBU_NET_NAME=%USERNAME%"
set /p "SSBU_NET_NAME=Your player name [%SSBU_NET_NAME%]: "
set "SSBU_NET_ROOM=smash"
set /p "SSBU_NET_ROOM=Room name - the same as the host's [smash]: "
start "SSBU netplay log" /min cmd /c "build\ssbu.exe dump\modules --status 30 > build\play.log 2>&1"
echo.
echo Starting the game, joining room "%SSBU_NET_ROOM%" at %SSBU_NET_RELAY%...
echo In the game: Local Wireless, then pick the host's room from the Rooms List.
echo The overlay (F2) shows whether the relay is reached and the ping to it.
echo.
pause
