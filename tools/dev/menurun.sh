#!/bin/sh
# Boot, go title -> main menu -> ... with scripted input (menus only, no battle), capture frames
# and log frames' draws. Runs on a copy of the save (<tree>/build/testdata), so a game the user has
# open is never written under.
# Usage: tools/dev/menurun.sh <out dir> [seconds]   (SSBU_INPUT, SSBU_GFX_LOG_AT, ... from the environment;
#        SSBU_TREE picks another folder laid out like this one - this one by default;
#        SSBU_EXE picks the executable, <tree>/build/ssbu.exe by default)
# Frames go to <out dir> (one a second), the log to <out dir>.log.
out=$1
secs=${2:-24}
tree=${SSBU_TREE:-.}
exe=${SSBU_EXE:-$tree/build/ssbu.exe}
rm -rf "$out" && mkdir -p "$out"
rm -rf "$tree/build/testdata" && mkdir -p "$tree/build/testdata" && cp -r "$tree/save" "$tree/build/testdata/" 2>/dev/null
SSBU_DATA_ROOT=$tree/build/testdata \
SSBU_INPUT="${SSBU_INPUT:-9:A,13:A,15.5:A}" \
SSBU_GFX_DEBUG="${SSBU_GFX_DEBUG:-16}" SSBU_GFX_LOG_AT="${SSBU_GFX_LOG_AT:-12,20}" \
SSBU_CAPTURE="$out" SSBU_CAPTURE_EVERY=1 SSBU_CAPTURE_MAX="${SSBU_CAPTURE_MAX:-40}" \
    timeout "$secs" "./$exe" "$tree/dump/modules" --status 30 > "$out.log" 2>&1
grep -c '^\[draw\] .* rt0 ' "$out.log"
grep -A10 'host exception\|stopped during' "$out.log" | head -14
