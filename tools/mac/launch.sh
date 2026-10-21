#!/bin/bash
# The macOS launchers' shared part: Play SSBU.command, launchers/*.command and the Play SSBU app the
# installer makes (~/Applications) run it, as Play SSBU.bat and launchers\*.bat do on Windows.
#
#   launch.sh [--gui] play|offline|host|join|unlock|build [arguments for unlock_all.py or build_game.py]
#
#   play     the game, online: it joins the lobby online_server.txt names, where Local Wireless lists
#            everyone's rooms (docs/MULTIPLAYER.md), as the name in online_name.txt (the game picks
#            one when there is none); off in online_direct.txt keeps a room's traffic going through
#            the lobby. The game reads those files itself (--online), so the settings menu's changes
#            count when it starts again. If the lobby does not answer, the game plays offline
#   offline  the game without going online
#   host     the game as the host of a private room: it runs the relay the others join (UDP port
#            39420, opened on the router with UPnP when the router allows it); the address to give
#            them shows here, from netplay_address.txt, and in the overlay (F2)
#   join     the game joining a host's room (or a standalone relay, tools/ssbu_relay.py)
#   unlock   tools/unlock_all.py on the game's save, with the game closed
#   build    tools/build_game.py with WindowSmash's Python (launchers\Build WindowSmash.bat)
#
# The game is build-mac/WindowSmash.app, started with open (so macOS treats it as the app it is: its
# own Dock icon, privacy permissions and Game Mode) from this folder, with dump/modules; its log goes
# to build/play.log, which the Terminal window shows as it goes. --gui (the Play SSBU app): questions
# and messages in dialogs, and no log window. SSBU_TREE names another folder laid out like this one
# to start the game of instead (tools/mac/boottest's made-up game).
# (Written for macOS's own bash, 3.2.)

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
root="$(cd "$here/../.." && pwd -P)"
gui=0

# Something to say: in this window, or in a dialog (--gui)
message() {
    if [ "$gui" = 1 ]; then
        osascript - "${2:-WindowSmash}" "$1" >/dev/null 2>&1 <<'EOF'
on run argv
    activate
    display alert (item 1 of argv) message (item 2 of argv) buttons {"OK"} default button "OK"
end run
EOF
    else
        echo "$1"
    fi
}

# What the Terminal window shows as the launcher goes (nothing with --gui)
say() {
    [ "$gui" = 1 ] || echo "$1"
}

# A line of text, with a default (empty: none): asked in this window, or in a dialog (--gui). Prints
# the answer; returns 1 when the dialog is cancelled.
ask() {
    local answer=
    if [ "$gui" = 1 ]; then
        answer=$(osascript - "$1" "$2" 2>/dev/null <<'EOF'
on run argv
    activate
    try
        set r to display dialog (item 1 of argv) default answer (item 2 of argv) with title "WindowSmash" buttons {"Cancel", "OK"} default button "OK" cancel button "Cancel"
        return "=" & text returned of r
    on error number -128
        return ""
    end try
end run
EOF
)
        [ -n "$answer" ] || return 1
        answer=${answer#=}
    else
        read -r -p "$1${2:+ [$2]}: " answer || echo >&2
    fi
    printf '%s\n' "${answer:-$2}"
}

# One of a list (--gui): prints the item picked; returns 1 when cancelled
choose_one() {
    local answer
    answer=$(osascript - "$@" 2>/dev/null <<'EOF'
on run argv
    activate
    set r to choose from list (items 3 thru -1 of argv) with prompt (item 1 of argv) with title "WindowSmash" default items {item 2 of argv}
    if r is false then return ""
    return "=" & item 1 of r
end run
EOF
)
    [ -n "$answer" ] || return 1
    printf '%s\n' "${answer#=}"
}

# The Windows launchers' pause: the window stays until Return (in Terminal only)
pause() {
    if [ "$gui" != 1 ] && [ -t 0 ]; then
        echo
        read -r -p "Press Return to close this window. " _
    fi
}

fail() {
    message "$1"
    pause
    exit 1
}

lower() {
    printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}

# What Play SSBU.bat clears (host and join keep the room and name variables: they ask for them)
clear_net() {
    unset SSBU_DATA_ROOT SSBU_INPUT SSBU_NET_HOST SSBU_NET_RELAY SSBU_NET_BIND "$@"
}

# The folder of the game to start ($tree: this one, or SSBU_TREE's), with the game built in it
game_folder() {
    case "${SSBU_TREE:-.}" in
        .) tree=$root ;;
        /*) tree=$SSBU_TREE ;;
        *) tree="$root/$SSBU_TREE" ;;
    esac
    [ -x "$tree/build-mac/WindowSmash.app/Contents/MacOS/ssbu" ] ||
        fail "The game isn't built here yet: double-click Install WindowSmash.command (in $root) to build it."
}

# The game to start ($tree), built and with its game data where its romfs.cfg says
pick_game() {
    local cfg path
    game_folder
    cfg="$tree/dump/modules/romfs.cfg"
    [ -f "$cfg" ] || fail "The game is not set up yet ($cfg is missing): double-click Install WindowSmash.command."
    while IFS= read -r path || [ -n "$path" ]; do
        path=${path%$'\r'}
        [ -n "$path" ] && [ ! -f "$path" ] || continue
        if ls -d "$path" 2>&1 | grep -q "not permitted"; then
            fail "macOS does not let the launcher read the game data ($path). Allow it in System Settings > Privacy & Security > Files and Folders (or Full Disk Access), then start it again."
        fi
        fail "The game data is not where it was when the game was built: $path. Make it reachable there again (a disk that is not connected?), or double-click Install WindowSmash.command to set it up again from where it is now."
    done < <(if [ "$(head -n 1 "$cfg" | tr -d '\r')" = layered ]; then tail -n +2 "$cfg"; else head -n 1 "$cfg"; fi)
}

# Online play (docs/MULTIPLAYER.md): what the game reads with --online, said here. The lobby is the
# first line of online_server.txt that is not a comment - host:port, then an optional lobby code
# (only players with the same code see each other's rooms) - or "off"; the name the other players
# see is in online_name.txt (the game picks one and keeps it there when there is none).
go_online() {
    local relay code rest name
    if [ -f online_server.txt ]; then
        while read -r relay code rest || [ -n "$relay" ]; do
            relay=${relay%$'\r'} code=${code%$'\r'}
            case "$relay" in
                '' | '#'*) relay= code= ; continue ;;
            esac
            break
        done < online_server.txt
        [ "$(lower "$relay")" = off ] && relay= code=
    fi
    if [ -z "$relay" ]; then
        say "Playing offline (no lobby in online_server.txt)."
        return 0
    fi
    if [ -f online_name.txt ]; then
        while IFS= read -r name || [ -n "$name" ]; do
            name=${name%$'\r'}
            [ -n "$name" ] && break
        done < online_name.txt
    fi
    say "Online: lobby $relay${code:+ (lobby code $code)}, as ${name:-the name the game picks}."
    say "To change them: Esc in the game. To play offline: launchers/Play SSBU Offline.command"
    return 0
}

# Starts the game (with the arguments given, after its own): open -W, in the background ($opener),
# waits for it to close. The environment's SSBU_* variables go to the game (open passes those it is
# given), with where the port is (the game starts in it: its settings, screenshots/ and reports/ are
# there) and where its log goes.
launch() {
    local app="$tree/build-mac/WindowSmash.app" name
    local args
    log="$tree/build/play.log"
    opened="$tree/build/open.txt"
    mkdir -p "$tree/build"
    rm -f "$log" "$opened"
    args=(-n -W --env "SSBU_CWD=$root" --env "SSBU_LOG=$log")
    for name in $(compgen -e); do
        case "$name" in
            SSBU_CWD | SSBU_LOG) ;;
            SSBU_*) args+=(--env "$name=${!name}") ;;
        esac
    done
    open "${args[@]}" -a "$app" --args "$tree/dump/modules" --status 30 "$@" 2> "$opened" &
    opener=$!
    trap 'kill $tailer $opener 2>/dev/null; exit 130' INT TERM HUP
}

# Until the game closes: its log in this window as it goes (not with --gui), then whether it stopped
# with an error
follow() {
    local status stopped
    tailer=
    if [ "$gui" != 1 ]; then
        echo "The game's log follows (it is also in $log). Closing this window leaves the game running."
        echo
        tail -n +1 -F "$log" 2>/dev/null &
        tailer=$!
    fi
    wait "$opener"
    status=$?
    if [ -n "$tailer" ]; then
        sleep 1
        kill "$tailer" 2>/dev/null
        wait "$tailer" 2>/dev/null
    fi
    trap - INT TERM HUP
    if [ "$status" != 0 ]; then
        fail "The game could not be started: $(cat "$opened" 2>/dev/null)"
    fi
    # (open's status tells little: 0 when the game stopped at once, or with "Unable to block on
    # application". The game's log is there once it started - with why, if it gave up early)
    if [ ! -s "$log" ]; then
        fail "The game did not start. $(cat "$opened" 2>/dev/null)"
    fi
    rm -f "$opened"
    stopped=$(grep -m 1 -E '^\*\*\* (stopped during|host exception)' "$log" 2>/dev/null)
    if [ -n "$stopped" ]; then
        message "The game stopped with an error: ${stopped#\*\*\* }. Its report is in $root/reports, and its log in $log." "The game stopped"
        pause
    fi
}

mode_play() {
    clear_net SSBU_NET_ROOM SSBU_NET_NAME SSBU_NET_DIRECT SSBU_NET_PORT
    pick_game
    go_online
    launch --online
    say "Starting the game..."
    follow
}

mode_offline() {
    clear_net SSBU_NET_ROOM SSBU_NET_NAME SSBU_NET_DIRECT SSBU_NET_PORT
    pick_game
    launch
    say "Starting the game, offline..."
    follow
}

mode_host() {
    local i
    clear_net
    pick_game
    say "Every player needs game version 13.0.5, as you have."
    SSBU_NET_NAME=$(ask "Your player name" "${USER:-$(id -un)}") || exit 0
    SSBU_NET_ROOM=$(ask "Room name - the other players use the same one" smash) || exit 0
    SSBU_NET_PORT=$(ask "UDP port" 39420) || exit 0
    SSBU_NET_HOST=1
    export SSBU_NET_NAME SSBU_NET_ROOM SSBU_NET_PORT SSBU_NET_HOST
    rm -f "$tree/netplay_address.txt"
    launch
    say ""
    say "Starting the game as host of room \"$SSBU_NET_ROOM\"..."
    say "In the game: Local Wireless > Create a Room. The others pick your room from the list."
    say ""
    for ((i = 0; i < 20; i++)); do
        [ -f "$tree/netplay_address.txt" ] && break
        sleep 1
    done
    if [ -f "$tree/netplay_address.txt" ]; then
        if [ "$gui" = 1 ]; then
            message "$(cat "$tree/netplay_address.txt")" "The address to give the other players"
        else
            cat "$tree/netplay_address.txt"
        fi
    else
        say "(The address to share will show in the game's overlay - press F2.)"
    fi
    say ""
    follow
}

mode_join() {
    clear_net
    pick_game
    say "The host needs game version 13.0.5 too."
    SSBU_NET_RELAY=$(ask "Host's address (IP or IP:port, from the host's netplay_address.txt)" "") || exit 0
    [ -n "$SSBU_NET_RELAY" ] || fail "An address is needed - ask the host for theirs."
    SSBU_NET_NAME=$(ask "Your player name" "${USER:-$(id -un)}") || exit 0
    SSBU_NET_ROOM=$(ask "Room name - the same as the host's" smash) || exit 0
    export SSBU_NET_RELAY SSBU_NET_NAME SSBU_NET_ROOM
    launch
    say ""
    say "Starting the game, joining room \"$SSBU_NET_ROOM\" at $SSBU_NET_RELAY..."
    say "In the game: Local Wireless, then pick the host's room from the Rooms List."
    say "The overlay (F2) shows whether the relay is reached and the ping to it."
    say ""
    follow
}

# $py: WindowSmash's Python (deps/venv, which the installer makes: 3.10 or newer, as arm64), or a
# message saying how to get it
tools_python() {
    py=deps/venv/bin/python
    if [ -x "$py" ] && "$py" -c 'import platform, sys; sys.exit(not (sys.version_info >= (3, 10) and platform.machine() == "arm64"))' >/dev/null 2>&1; then
        return 0
    fi
    if [ -x "$py" ]; then
        fail "WindowSmash's Python (deps/venv) is one the tools cannot use (they need Python 3.10 or newer, as arm64): double-click Install WindowSmash.command, which makes it again."
    fi
    fail "WindowSmash's Python (deps/venv) is not set up yet: double-click Install WindowSmash.command, which sets everything up."
}

mode_unlock() {
    local py status
    gui=0
    game_folder
    tools_python
    if pgrep -x ssbu >/dev/null 2>&1; then
        fail "Close every SSBU game window before unlocking fighters, then run this again."
    fi
    "$py" tools/unlock_all.py --root "$tree" "$@"
    status=$?
    pause
    exit "$status"
}

mode_build() {
    local py status
    gui=0
    tools_python
    # (kept awake: a build of an hour or more is not stopped by the Mac going to sleep)
    caffeinate -i "$py" tools/build_game.py "$@"
    status=$?
    pause
    exit "$status"
}

main() {
    local mode
    if [ "$1" = --gui ]; then
        gui=1
        shift
    fi
    mode=$1
    shift
    cd "$root" || exit 1
    case "$mode" in
        play | offline | host | join | unlock | build) "mode_$mode" "$@" ;;
        *)
            echo "Usage: $0 [--gui] play|offline|host|join|unlock|build [arguments]"
            exit 2
            ;;
    esac
}

# (sourced, as the tests do: only the functions)
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
