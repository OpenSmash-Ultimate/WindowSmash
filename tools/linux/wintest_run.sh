#!/usr/bin/env bash
# A menu-only run of a test build (wintest.py) or the Windows build, for comparing renderers:
# title -> main menu -> character select (no match), frames captured every 3 s, then closed.
# Uses copies of the save and shader cache, so the port's own are never written.
#
# Usage: wintest_run.sh <exe> <name> [shader cache to start from: copy|empty]
#   <name>: a folder under linux-toolchain/test/runs for the log and captures
#   WINTEST_PORT=<game tree> for another tree's modules, save and cache (default: the port's own,
#   as tools/linux/toolchain.py finds it)
set -u
EXE="$1"; NAME="$2"; CACHE="${3:-copy}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OWN="$HERE/../../../native-port"
[ -d "$HERE/../../dump" ] && OWN="$HERE/../.."
PORT="$(cd "${WINTEST_PORT:-$OWN}" && pwd)"
RUN="$(cd "${SSBU_LINUX_TOOLCHAIN:-$HERE/../../../linux-toolchain}" && pwd)/test/runs/$NAME"
rm -rf "$RUN"; mkdir -p "$RUN/root" "$RUN/shots" "$RUN/cache"
cp -r "$PORT/save" "$RUN/root/save"
# (as the Linux package ships it: what the game has used, dated as it is - the start's prewarm
# makes every file dated after 2002, and the ahead-of-time compiler's are dated 2001)
[ "$CACHE" = copy ] && find "$PORT/dump/shadercache" -maxdepth 1 -name 'v10_*' -newermt 2002-01-01 -exec cp -p -t "$RUN/cache/" {} + 2>/dev/null
cd "$RUN"
export SSBU_DATA_ROOT="$(cygpath -w "$RUN/root")" SSBU_SHADER_CACHE="$(cygpath -w "$RUN/cache")" SSBU_CAPTURE="$(cygpath -w "$RUN/shots")"
export SSBU_CAPTURE_EVERY=3 SSBU_CAPTURE_MAX=16 SSBU_INPUT="9:A,12:X,14:X,17:A,19.5:A,22.5:A,25.5:A" SSBU_NET_UPNP=0
"$EXE" "$(cygpath -w "$PORT/dump/modules")" --status 15 > "$RUN/play.log" 2>&1 &
PID=$!
sleep 50
kill "$PID" 2>/dev/null; sleep 1; taskkill //F //PID "$(cat /proc/$PID/winpid 2>/dev/null)" >/dev/null 2>&1
echo "$RUN: $(ls "$RUN/shots" | wc -l) captures, $(ls "$RUN/cache" | wc -l) cached shaders"
grep -E "\[gfx\]|failed|FAILED|\*\*\*|stopped" "$RUN/play.log" | head -30
