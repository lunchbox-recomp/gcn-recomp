#!/bin/sh
# Builds msl_spike and dump_glsl against a tree's runtime/gx sources.
#
#   build.sh [BUILD_DIR [PROGRAM...]]
#
#   BUILD_DIR   default: <repo>/build/msl_check (build*/ is git-ignored)
#   PROGRAM     msl_spike, dump_glsl or both; default both
#
# SHADERGEN_ROOT is the tree whose runtime/gx/shadergen.cpp and headers the programs build
# against; the default is this one. Only that tree's runtime is on the include path, so
# keys_io.h's "gx/shadergen.h" is its header too. msl_spike needs gen_pixel_body,
# gen_vertex_body and pixel_io, so against a tree older than those build dump_glsl alone.
#
# macOS only: it links Metal and Apple's OpenGL. SDK picks the sysroot; the default is the
# 26.5 one, because the default SDK may not link against the Command Line Tools in use.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SG="$(cd "${SHADERGEN_ROOT:-$ROOT}" && pwd)"
OUT="${1:-$ROOT/build/msl_check}"
if [ $# -gt 0 ]; then shift; fi
if [ $# -eq 0 ]; then set -- msl_spike dump_glsl; fi
SDK="${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk}"
mkdir -p "$OUT"

CXXFLAGS="-std=c++20 -O1 -fobjc-arc -isysroot $SDK -I$SG/runtime/gx -I$SG/runtime -I$HERE/src"

SRC="$HERE/src"
for prog in "$@"; do
  case "$prog" in
  msl_spike)
    clang++ $CXXFLAGS \
        "$SRC/main.mm" "$SRC/cmd_compile.mm" "$SRC/cmd_compare.mm" "$SRC/cmd_glcompile.cpp" "$SRC/cmd_synth.cpp" \
        "$SRC/msl_translate.cpp" "$SRC/metal_util.mm" "$SRC/mtl_draw.mm" "$SRC/gl_ref.cpp" \
        "$SRC/test_inputs.cpp" "$SRC/synth_keys.cpp" "$SRC/spike_util.cpp" "$SG/runtime/gx/shadergen.cpp" \
        -framework Foundation -framework Metal -framework OpenGL -o "$OUT/msl_spike"
    ;;
  dump_glsl)
    clang++ $CXXFLAGS "$SRC/dump_glsl.cpp" "$SG/runtime/gx/shadergen.cpp" -o "$OUT/dump_glsl"
    ;;
  *)
    echo "build.sh: no program $prog (msl_spike or dump_glsl)" >&2
    exit 2
    ;;
  esac
  echo "built $OUT/$prog against $SG/runtime/gx"
done
