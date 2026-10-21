#!/bin/bash
# Starts the game as the netplay host: it runs the relay the other players join (UDP port 39420,
# opened on the router with UPnP when the router allows it). See docs/MULTIPLAYER.md. The address to
# give the other players appears here, in the game's overlay (F2) and in netplay_address.txt; the
# log goes to build/play.log and this window. Every player needs game version 13.0.5.
# (tools/mac/launch.sh does the work.)
exec /bin/bash "$(cd "$(dirname "$0")/.." && pwd -P)/tools/mac/launch.sh" host "$@"
