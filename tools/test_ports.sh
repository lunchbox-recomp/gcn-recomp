#!/bin/bash
# Runs the gamepad port checks: the port checks, a recording of two gamepads, and that
# recording replayed with none, and that recording's log replayed again with its SRAM
# snapshot taken away, then an interrupt handled beside the main loop. The port checks
# include the ports remembered in saves/ports.txt under the work dir. SDL's virtual
# joysticks stand in for the controllers, so no joystick may be connected.
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
mkdir -p "$work/files" "$work/log" "$work/interrupt" || exit 2
status=0
export SDL_AUDIO_DRIVER=dummy
"$test_ports" "$work/files" || status=1
"$test_ports" --record "$work/log" || status=1
"$test_ports" --replay "$work/log" || status=1
"$test_ports" --replay-no-sram "$work/log" || status=1
"$test_ports" --interrupt "$work/interrupt" || status=1
exit $status
