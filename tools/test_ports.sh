#!/bin/bash
# Runs the gamepad port checks: the port checks, a recording of two gamepads, and that
# recording replayed with none, and that recording's log replayed again with its SRAM
# snapshot taken away, then a version 4 log replayed, logs recorded at disc console and at
# an unknown speed replayed, an interrupt handled beside the main loop, the frontend under
# --no-gamepads, the game resetting the console, the game shutting it down, a restart and a
# quit asked for from the frontend with the game resetting the console as each ends, and a
# restart asked for in a replay. The port
# checks include the ports remembered in saves/ports.txt under the work dir. SDL's virtual
# joysticks stand in for the controllers. On macOS and Linux test_ports turns off SDL's
# drivers that find real ones (kRealDriversOff in tools/test_ports.cpp) and clears
# SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD, which SDL reads ahead of the allow list,
# to leave a controller that's connected alone. This script sets that variable, so that the
# port checks see it cleared.
#
# usage: tools/test_ports.sh <test_ports> <work dir>
#   <test_ports>  the program `ninja test_ports` builds in a build directory of a game
#                 project (from tools/test_ports.cpp)
#   <work dir>    where the checks put their files; it must not exist
# Exits 0 only if every check passes.
test_ports=$1
work=$2
if [ ! -x "$test_ports" ] || [ -z "$work" ]; then
  echo "usage: tools/test_ports.sh <test_ports> <work dir>" >&2
  exit 2
fi
if [ -e "$work" ]; then
  echo "$work exists: give a new directory" >&2
  exit 2
fi
mkdir -p "$work/files" "$work/log" "$work/v4" "$work/console" "$work/interrupt" \
  "$work/no-gamepads" "$work/reset" "$work/shutdown" "$work/restart" "$work/quit" "$work/restart-replay" || exit 2
status=0
export SDL_AUDIO_DRIVER=dummy
export SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1
"$test_ports" "$work/files" || status=1
"$test_ports" --record "$work/log" || status=1
"$test_ports" --replay "$work/log" || status=1
"$test_ports" --replay-no-sram "$work/log" || status=1
"$test_ports" --replay-v4 "$work/v4" || status=1
"$test_ports" --replay-console "$work/console" || status=1
"$test_ports" --interrupt "$work/interrupt" || status=1
"$test_ports" --no-gamepads "$work/no-gamepads" || status=1
"$test_ports" --reset "$work/reset" || status=1
"$test_ports" --shutdown "$work/shutdown" || status=1
"$test_ports" --restart "$work/restart" || status=1
"$test_ports" --quit "$work/quit" || status=1
"$test_ports" --restart-replay "$work/restart-replay" || status=1
exit $status
