#!/bin/sh
# Builds msl_spike and dump_glsl against this repository's runtime/gx sources.
#
#   build.sh [BUILD_DIR]       default: <repo>/build/msl_check (build*/ is git-ignored)
#
# macOS only: it links Metal and Apple's OpenGL. SDK picks the sysroot; the default is the
# 26.5 one, because the default SDK may not link against the Command Line Tools in use.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$ROOT/build/msl_check}"
SDK="${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk}"
mkdir -p "$OUT"

CXXFLAGS="-std=c++20 -O1 -fobjc-arc -isysroot $SDK -I$ROOT/runtime/gx -I$ROOT/runtime -I$HERE/src"

clang++ $CXXFLAGS "$HERE/src/msl_spike.mm" "$ROOT/runtime/gx/shadergen.cpp" \
    -framework Foundation -framework Metal -framework OpenGL -o "$OUT/msl_spike"
clang++ $CXXFLAGS "$HERE/src/dump_glsl.cpp" "$ROOT/runtime/gx/shadergen.cpp" -o "$OUT/dump_glsl"
echo "built $OUT/msl_spike and $OUT/dump_glsl"
