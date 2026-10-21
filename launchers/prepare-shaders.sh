#!/usr/bin/env bash
# Compiles the game's shaders ahead of time (tools/shader_aot.py), as Prepare Shaders.bat does: an
# effect you have not seen yet then shows at once the first time, instead of a moment later.
# Run it once after the build. It takes from minutes to an hour or
# two, at low priority; stop it with Ctrl+C and run it again: what is done stays done.
# Needs the game built (bash install-windowsmash.sh) and the game files where they were.
root="$(dirname "$(dirname "$(readlink -f "$0")")")"   # (the folder above this one)
py="$root/deps/venv/bin/python"
if [ ! -x "$py" ] || [ ! -f "$root/dump/modules/romfs.cfg" ]; then
    echo "WindowSmash is not set up here yet: run  bash install-windowsmash.sh  first."
    exit 1
fi
cd "$root" || exit 1
# (keeps the PC from going to sleep meanwhile: a Steam Deck does after a few minutes)
if command -v systemd-inhibit >/dev/null 2>&1; then
    exec systemd-inhibit --what=idle:sleep --who=WindowSmash --why="Compiling the game's shaders" \
        "$py" -u tools/shader_aot.py "$@"
fi
exec "$py" -u tools/shader_aot.py "$@"
