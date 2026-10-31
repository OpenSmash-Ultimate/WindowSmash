@echo off
setlocal
rem [rollback research build: build\ssbu_rollback.exe - see patches\rollback\README.md and TWO_PC_SETUP.md]
rem 2-4 PCs play one match with rollback, each with its own controller. One PC hosts; the others join it with the
rem host's address. The host gives each joining game its player number and passes every player's moves on to the rest.
set "SSBU_DATA_ROOT="
set "SSBU_INPUT="
cd /d "%~dp0.."
if not exist build\ssbu_rollback.exe (echo build\ssbu_rollback.exe is missing. & pause & exit /b 1)
if not exist dump\modules\romfs.cfg (echo dump\modules\romfs.cfg is missing: the game files aren't here. & pause & exit /b 1)

:ask_role
set "ROLE="
set /p "ROLE=Host or join? (H/J): "
if /i "%ROLE%"=="H" goto host
if /i "%ROLE%"=="J" goto join
goto ask_role

:host
set "SSBU_RB_NET_PLAYERS="
set /p "SSBU_RB_NET_PLAYERS=How many players, this PC's included? (2-4): "
if not "%SSBU_RB_NET_PLAYERS%"=="2" if not "%SSBU_RB_NET_PLAYERS%"=="3" if not "%SSBU_RB_NET_PLAYERS%"=="4" goto host
set "LOCAL=0" & set "MYPORT=7701" & set "PEERPORT=7702" & set "PEER=*" & set "LOG=host"
goto start

:join
echo.
echo Type the address the host's launcher shows (on the same network, the one for this network).
set "PEER="
set /p "PEER=The host's address: "
if defined PEER set "PEER=%PEER:"=%"
if defined PEER set "PEER=%PEER: =%"
if not defined PEER goto join
rem (port 0: any free one - joining games only call out, and several can run on one PC)
set "LOCAL=-1" & set "MYPORT=0" & set "PEERPORT=7701"
rem (each joining game's own log: another game on this PC may still be writing build\play_rollback_net_join.log)
set "LOG=join"
for %%n in (join join2 join3 join4) do (
    set "LOG=%%n"
    if not exist "build\play_rollback_net_%%n.log" goto start
    2>nul (>>"build\play_rollback_net_%%n.log" call ) && goto start
)

:start
rem The game files' locations are written into dump\modules\romfs*.cfg as full paths: point them at this folder.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$r=(Get-Location).Path; foreach ($f in 'dump\modules\romfs.cfg','dump\modules\romfs_base.cfg') { if (Test-Path $f) { $t = Get-Content $f | ForEach-Object { $i = $_.IndexOf('\dump\modules\'); if ($i -ge 0) { $r + $_.Substring($i) } else { $_ } }; Set-Content -Encoding ascii $f $t } }"

set "SSBU_RB=1"
rem net,<player: 0 the host, -1 a joiner (the host gives the number)>,<this PC's port>,<the other's port>,<input delay, -1 auto>,<frames checked>,<the host's address, * to host>
rem (tests: "set RB_DELAY=<n>" fixes the delay on the host; "set SSBU_RB_NET_LIVE=0" plays this PC's player by a bot)
if not defined RB_DELAY set "RB_DELAY=auto"
set "DLY=%RB_DELAY%"
if /i "%RB_DELAY%"=="auto" set "DLY=-1"
set "SSBU_RB_NET=net,%LOCAL%,%MYPORT%,%PEERPORT%,%DLY%,7200,%PEER%"
if not defined SSBU_RB_NET_LIVE set "SSBU_RB_NET_LIVE=1"
rem (every PC: its controller and keyboard are its player 1; a joining game moves them to the player the host gives it)
set "SSBU_CONTROLLERS=%CD%\controllers_rollback_p1.ini"
echo.
if "%LOCAL%"=="0" (echo This PC hosts %SSBU_RB_NET_PLAYERS% players. The game asks the router to open UDP port 7701 ^(UPnP^).) else (echo This PC joins the host at %PEER%: the host gives it its player number.)
echo The log goes to build\play_rollback_net_%LOG%.log.
if exist build\rollback_address.txt if "%LOCAL%"=="0" del build\rollback_address.txt
start "SSBU rollback netplay" /min cmd /c "build\ssbu_rollback.exe dump\modules --status 30 > build\play_rollback_net_%LOG%.log 2>&1"
if not "%LOCAL%"=="0" exit /b 0
rem The host: what its router said, and the addresses to give the others (build\rollback_address.txt, written by the game).
echo.
echo Asking the router to open the port, once the game has started (up to a minute and a half)...
for /l %%i in (1,1,90) do (
    if exist build\rollback_address.txt goto show_address
    ping -n 2 127.0.0.1 >nul
)
echo The game has not said yet: see build\rollback_address.txt or the log in a minute.
pause
exit /b 0
:show_address
echo.
type build\rollback_address.txt
echo.
echo (also in build\rollback_address.txt)
pause
