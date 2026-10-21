#!/bin/bash
# Starts the native Super Smash Bros. Ultimate offline (Play SSBU.command plays online). Its log
# goes to build/play.log and this window. (tools/mac/launch.sh does the work.)
exec /bin/bash "$(cd "$(dirname "$0")/.." && pwd -P)/tools/mac/launch.sh" offline "$@"
