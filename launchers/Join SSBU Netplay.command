#!/bin/bash
# Starts the game as a netplay player joining a host's relay (or a standalone relay,
# tools/ssbu_relay.py). No port forwarding needed here. See docs/MULTIPLAYER.md. The log goes to
# build/play.log and this window. The host needs game version 13.0.5 too. (tools/mac/launch.sh does
# the work.)
exec /bin/bash "$(cd "$(dirname "$0")/.." && pwd -P)/tools/mac/launch.sh" join "$@"
