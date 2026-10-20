#!/bin/sh
# Every measurement of the spike, in one log on stdout.
#
#   run_all.sh BUILD_DIR WORK_DIR GAME_KEYS.bin [ROUTE_KEYS.bin [N N N N N N N N N N]]
#
#   BUILD_DIR     where build.sh put msl_spike
#   WORK_DIR      scratch space for the MSL files, the error lists and the made-up keys
#   GAME_KEYS.bin the whole key list (see README.md for making one)
#   ROUTE_KEYS    a smaller list for the ten-key and logic-op runs; default GAME_KEYS
#   N...          the ten key indices of ROUTE_KEYS to compare; default 0 to 9
#
# The made-up keys (seed 7, 300 random ones after the directed ones) are made here.
set -e
BUILD="$1"
WORK="$2"
GAME="$3"
ROUTE="${4:-$GAME}"
if [ $# -lt 3 ]; then
  echo "usage: $0 BUILD_DIR WORK_DIR GAME_KEYS.bin [ROUTE_KEYS.bin [N x10]]" >&2
  exit 2
fi
if [ $# -gt 4 ]; then shift 4; else set -- 0 1 2 3 4 5 6 7 8 9; fi
SPIKE="$BUILD/msl_spike"
mkdir -p "$WORK/msl"
SYNTH="$WORK/synth.bin"
{
  echo "### synthetic keys (made by msl_spike synth, seed 7)"
  "$SPIKE" synth "$SYNTH" 7 300
  echo "### emit"
  "$SPIKE" emit "$GAME" "$WORK/msl"
  echo "### compile, game keys, cold, fast math off"
  "$SPIKE" compile "$GAME" cold "$WORK/errors-keys.txt"
  echo "### compile, game keys, cold, fast math on"
  "$SPIKE" compile "$GAME" cold "$WORK/errors-keys-fast.txt" fast
  echo "### compile, game keys, warm (second pass of the same sources)"
  "$SPIKE" compile "$GAME" warm "$WORK/errors-warm.txt" >/dev/null || true
  "$SPIKE" compile "$GAME" warm "$WORK/errors-warm.txt"
  echo "### compile, synthetic keys, cold"
  "$SPIKE" compile "$SYNTH" cold "$WORK/errors-synth.txt"
  echo "### parallel, game keys, cold"
  "$SPIKE" parallel "$GAME"
  echo "### GL baseline, game keys"
  "$SPIKE" glcompile "$GAME" 2>/dev/null
  echo "### floor"
  "$SPIKE" floor
  echo "### ten keys, fast math off"
  "$SPIKE" compare "$ROUTE" "$@" || true
  echo "### ten keys, fast math on"
  "$SPIKE" compare "$ROUTE" --fastmath "$@" || true
  echo "### ten keys, fast math off, no fog"
  "$SPIKE" compare "$ROUTE" --nofog "$@" || true
  echo "### all game keys, fast math off"
  "$SPIKE" compare "$GAME" --quiet all || true
  echo "### all game keys, fast math on"
  "$SPIKE" compare "$GAME" --quiet --fastmath all || true
  echo "### synthetic keys, indirect + z plane + screen coords, fast math off"
  "$SPIKE" compare "$SYNTH" --quiet --indirect --zplane --screen all || true
  echo "### synthetic keys, indirect + z plane + screen coords, fast math on"
  "$SPIKE" compare "$SYNTH" --quiet --indirect --zplane --screen --fastmath all || true
  echo "### logic op OR, route keys, fast math off"
  "$SPIKE" logic "$ROUTE" 7 --quiet all || true
  echo "### logic op OR, route keys, fast math on"
  "$SPIKE" logic "$ROUTE" 7 --quiet --fastmath all || true
  echo "### logic op COPY, route keys, fast math on"
  "$SPIKE" logic "$ROUTE" 3 --quiet --fastmath all || true
  echo "### negative control: gl_FragCoord flipped (z plane and screen keys must differ)"
  "$SPIKE" compare "$SYNTH" --zplane --screen --fastmath --flipfc 97 99 60 63 66 70 || true
  echo "### winding: GL CCW front, cull back; Metal CW then CCW, both vertex orders"
  "$SPIKE" compare "$ROUTE" --fastmath --cullback 0 | sed -n 3p
  "$SPIKE" compare "$ROUTE" --fastmath --cullback --ccw 0 | sed -n 3p
  "$SPIKE" compare "$ROUTE" --fastmath --cullback --reverse 0 | sed -n 3p
  "$SPIKE" compare "$ROUTE" --fastmath --cullback --reverse --ccw 0 | sed -n 3p
  echo "### review additions: fog curves 3 to 7, infinite and NaN fog inputs"
  for mode in 3 4 5 6 7; do
    "$SPIKE" compare "$ROUTE" --quiet --fastmath --fogmode=$mode all || true
  done
  for arg in --fogA=inf --fogA=nan --fogB=inf --fogB=nan; do
    "$SPIKE" compare "$ROUTE" --quiet --fastmath "$arg" all || true
  done
  echo "### review additions: bilinear sampling, clipped geometry"
  "$SPIKE" compare "$ROUTE" --quiet --fastmath --linear all || true
  "$SPIKE" compare "$ROUTE" --quiet --fastmath --clipz all || true
  "$SPIKE" compare "$ROUTE" --quiet --fastmath --zprojw0 all || true
  "$SPIKE" compare "$ROUTE" --quiet --fastmath --zprojnan all || true
  echo "### review additions: MTLMathMode Relaxed (1) and Fast (2), against GL, game keys"
  MATHMODE=1 "$SPIKE" compare "$GAME" --quiet all || true
  MATHMODE=2 "$SPIKE" compare "$GAME" --quiet all || true
  echo "### review additions: GL warm, the same sources with no nonce (OS cache holds them)"
  NONONCE=1 "$SPIKE" glcompile "$GAME" 2>/dev/null
  echo "### review additions: blend-only pipeline variants"
  "$SPIKE" blendvar "$GAME" fast
} 2>&1
