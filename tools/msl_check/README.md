# msl_check

A dev check, macOS only. It shows that shadergen's GLSL can feed Metal. The shader text from
`void main() {` on compiles as Metal Shading Language under a small prelude, with the
declarations and entry points written from the `ShaderKey`. The program began as the spike
that proved this, and its code is kept as it was, split into files that each do one job.

## What it proves

For any key list, with `gen_pixel_body`, `gen_vertex_body` and `pixel_io` from
`runtime/gx/shadergen.h`:

- every key's fragment program compiles in Metal and builds a render pipeline (`compile`)
- the same inputs drawn in Apple's GL (the real GLSL) and in Metal (the MSL translation) give
  the same pixels, within 1 level with fast math off and exactly with it on (`compare`)
- the GX logic op through framebuffer fetch matches `glLogicOp` (`logic`)
- what a cold compile costs, serially, in parallel and against GL (`compile`, `parallel`,
  `glcompile`, `floor`, `blendvar`)

It doesn't check blending, MSAA, points, mipmaps or the VR paths.

Measured on an M4 Max with macOS 26.6 and MSL 3.0, a game's 247 keys and 446 made-up ones
compile. The made-up ones reach every branch of `gen_pixel_shader`. Fast math on is bit-exact
against GL. A cold compile is 32 ms a key serially against GL's 11, and 3 ms a key in parallel.
Once the OS shader cache holds a program it costs about nothing.

## Files

Everything is in `src/`. The program is `msl_spike`, and `dump_glsl` is a second, separate one.

| File | Job |
|---|---|
| `main.mm` | `main()`, which picks a mode by name |
| `commands.h` | declares the modes |
| `cmd_compile.mm` | the Metal modes `emit`, `compile`, `blendvar`, `parallel` and `floor` |
| `cmd_glcompile.cpp` | the `glcompile` mode |
| `cmd_compare.mm` | the `compare` and `logic` modes, with their options and the image diff |
| `cmd_synth.cpp` | the `synth` mode, which writes the made-up key list |
| `synth_keys.h`, `synth_keys.cpp` | the generator of the made-up keys |
| `msl_translate.h`, `msl_translate.cpp` | the MSL prelude and the translation from a `ShaderKey` to MSL |
| `msl_blocks.h` | the uniform blocks, as C++ structs and as MSL text |
| `metal_util.h`, `metal_util.mm` | Metal compile options, library and pipeline helpers |
| `mtl_draw.h`, `mtl_draw.mm` | the Metal renderer of the comparison |
| `gl_ref.h`, `gl_ref.cpp` | the GL reference renderer of the comparison |
| `test_inputs.h`, `test_inputs.cpp` | the comparison's inputs and the image both renderers return |
| `spike_util.h`, `spike_util.cpp` | the clock, text file writing, key list loading and the timing summary |
| `keys_io.h` | reads caches and key lists, and writes key lists |
| `dump_glsl.cpp` | the `dump_glsl` program |

## Build

```
tools/msl_check/build.sh [BUILD_DIR]
```

`BUILD_DIR` defaults to `build/msl_check` (git-ignored). It builds two programs from this
tree's `runtime/gx/shadergen.cpp` with the 26.5 SDK sysroot,
`-isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk` (override with `SDK=`):

- `msl_spike` is the check (`main.mm` and the files it uses)
- `dump_glsl` writes each key's GLSL, to compare two builds of shadergen (`src/dump_glsl.cpp`)

## A key list

Key lists are game data and aren't committed. Make one from the `saves/shaders.bin` that
runs leave behind, with this repository's own `shader_keys` (`tools/shader_keys.cpp`). It
makes every key canonical, keeps each once in the order first seen and takes caches and key
lists alike:

```
cmake --build <desktop build dir> --target shader_keys
<desktop build dir>/shader_keys keys.bin run1/saves/shaders.bin run2/saves/shaders.bin
```

`dump_glsl KEYS_OUT.bin GLSL_DIR|- FEATURES.tsv|- IN.bin...` does the same union, and can also
write each key's GLSL (`<n>.frag`, `vertex.vert`) and a table of the features each key uses.

A key is named by its index in the list. The made-up keys aren't data. `msl_spike synth
OUT.bin 7 300` writes 146 directed keys and 300 random ones from seed 7.

## Run

All modes take a key list. `N...` are key indices, or `all`. The options come anywhere after
the key list in `compare` and `logic`.

```
msl_spike emit KEYS.bin MSL_DIR
```
Writes `<n>.metal` for every key and `vertex.metal`, to read the MSL.

```
msl_spike compile KEYS.bin [cold|warm [ERRORS.txt [fast]]]
```
Compiles every key (library, then pipeline) and times it. `cold` adds a nonce to each program
so the OS cache can't serve it. Without it, the sources are compiled as they are, so a second
run is warm. Errors go to `ERRORS.txt`. `fast` turns fast math on.

```
msl_spike parallel KEYS.bin
msl_spike glcompile KEYS.bin
msl_spike floor [HEADER_FILE...]
msl_spike blendvar KEYS.bin [fast]
```

- `parallel` compiles all keys at once through the async APIs.
- `glcompile` runs the same keys through GL (compile, link, first draw). With `NONONCE=1` in
  the environment it gives the warm figure.
- `floor` times an empty function under `metal_stdlib` and under the prelude, or under each
  given header file.
- `blendvar` times a second, third and fourth pipeline for the same fragment function when
  only blend state or the write mask differs.

```
msl_spike compare KEYS.bin [options] N...|all
msl_spike logic KEYS.bin OP [options] N...|all
```
Draws a full-screen triangle with each key in GL and in Metal into 256x256 RGBA8 plus Depth32F
and prints the largest difference per channel, the pixels that differ and the depth
difference. It exits 0 if every key is within 1 level. `logic` does the same over a filled
target with GX logic op `OP` (0 clear ... 7 or ... 15 set), through framebuffer fetch.

Options of `compare` and `logic`:

| Option | Does |
|---|---|
| `--fastmath` | fast math on in Metal (off by default) |
| `--quiet` | prints only keys that differ |
| `--nofog` | fog off |
| `--indirect` | indirect matrices and scales set |
| `--zplane` | a z freeze plane in window coordinates |
| `--screen` | EFB copy texgens take the fragment position |
| `--cullback`, `--ccw`, `--reverse` | cull back faces; Metal's front winding counter-clockwise; swap two vertices |
| `--flipfc` | negative control that flips `gl_FragCoord` y, so `--zplane` and `--screen` keys must differ |

More options:

| Option | Does |
|---|---|
| `--fogmode=N` | the fog curve, GXSetFog's type (`u_fogmode`; the default is 2, 3 to 7 are the others, +8 orthographic) |
| `--fogA=F`, `--fogB=F`, `--fogC=F`, `--fogw=F` | the four components of `u_fog`; `inf` and `nan` work |
| `--linear` | bilinear sampling instead of nearest |
| `--clipz` | two vertices' z moved out of the near and far range (0.6 and -1.6), so the triangle is clipped |
| `--zprojw0`, `--zprojnan` | the depth projection's w set to 0, or all of it to 0 |

Environment variables:

- `MATHMODE=N` sets `MTLMathMode` (0 safe, 1 relaxed, 2 fast) and overrides `--fastmath`. With
  `PRECISEFN=1` the floating-point functions are precise rather than fast.
- `NONONCE=1` leaves the nonce out of `glcompile`.

## Run everything

```
tools/msl_check/run_all.sh BUILD_DIR WORK_DIR GAME_KEYS.bin [ROUTE_KEYS.bin [N x10]]
```

Prints every measurement above in one log. `GAME_KEYS` is the whole list and `ROUTE_KEYS` a
smaller one for the ten-key and logic-op runs (the default is the same list). The ten indices
default to 0 to 9. `WORK_DIR` gets the MSL files, the error lists and the made-up keys.

## Checking a shadergen change

`runtime/gx/shadergen.cpp` must keep `gen_pixel_shader`'s output byte for byte. Build
`dump_glsl` against the old and the new source, dump the same key lists with
`dump_glsl OUT.bin DIR - IN.bin...` into two directories and `diff -r` them.
