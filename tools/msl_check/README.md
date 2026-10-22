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
  the same pixels, within 1 level with fast math off and exactly with it on, and the same
  depth within 1e-6 (`compare`)
- the GX logic op through framebuffer fetch matches `glLogicOp` exactly with fast math on
  (`logic`). With it off, the fog's 1-level differences go through the bitwise op, which can
  turn one level into many (up to 32 for OR and 63 for XOR on the made-up keys), so `logic`
  with those ops exits 1 there
- what a cold compile costs, serially, in parallel and against GL (`compile`, `parallel`,
  `glcompile`, `floor`, `blendvar`)

It doesn't check blending, MSAA, points, mipmaps or the VR paths.

Measured on an M4 Max with macOS 26.6 and MSL 3.0, a game's 247 keys and 446 made-up ones
compile. The made-up ones reach every branch of `gen_pixel_shader`. Fast math on gives
bit-exact colors against GL, and on the made-up keys depth within a step of 2^-24 (two with
`--clipz`). A cold compile is 32 ms a key serially against GL's 11, and 3 ms a key in parallel.
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
| `msl_blocks.h` | the uniform blocks as C++ structs, from macros that `msl_translate.cpp` also turns into MSL text |
| `metal_util.h`, `metal_util.mm` | Metal compile options, library and pipeline helpers |
| `mtl_draw.h`, `mtl_draw.mm` | the Metal renderer of the comparison |
| `gl_ref.h`, `gl_ref.cpp` | the GL reference renderer of the comparison |
| `test_inputs.h`, `test_inputs.cpp` | the comparison's inputs and the image both renderers return |
| `spike_util.h`, `spike_util.cpp` | the clock, text file writing, key list loading, the timing summary and the parsing of numeric arguments |
| `keys_io.h` | reads caches and key lists, and writes key lists |
| `dump_glsl.cpp` | the `dump_glsl` program |

## Build

```
tools/msl_check/build.sh [BUILD_DIR [PROGRAM...]]
```

`BUILD_DIR` defaults to `build/msl_check` (git-ignored). It builds two programs from this
tree's `runtime/gx/shadergen.cpp` with the 26.5 SDK sysroot,
`-isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk` (override with `SDK=`):

- `msl_spike` is the check (`main.mm` and the files it uses)
- `dump_glsl` writes each key's GLSL, to compare two builds of shadergen (`src/dump_glsl.cpp`)

Name `msl_spike` or `dump_glsl` after `BUILD_DIR` to build only that one. With
`SHADERGEN_ROOT=DIR` in the environment both build against `DIR/runtime/gx` instead of this
tree's, headers included. `msl_spike` needs `gen_pixel_body`, `gen_vertex_body` and
`pixel_io`, so a tree older than those can only build `dump_glsl`.

## A key list

Key lists are game data and aren't committed. Make one from the `saves/shaders.bin` that
runs leave behind, with this repository's own `shader_keys` (`tools/shader_keys.cpp`). It
makes every key canonical, keeps each once in the order first seen and takes caches and key
lists alike. From the repository root, this writes the list under `build/`, which git
ignores:

```
cmake --build <desktop build dir> --target shader_keys
mkdir -p build
<desktop build dir>/shader_keys build/keys.bin run1/saves/shaders.bin run2/saves/shaders.bin
```

`dump_glsl KEYS_OUT.bin GLSL_DIR|- FEATURES.tsv|- IN.bin...` does the same union, and can also
write each key's GLSL (`<n>.frag`, `vertex.vert`) and a table of the features each key uses.

A key is named by its index in the list. The made-up keys aren't data. `msl_spike synth
OUT.bin 7 300` writes 146 directed keys and 300 random ones from seed 7.

## Run

Every mode but `floor` and `synth` reads a key list. `N...` are key indices, or `all`. In
`compare` and `logic` the options and the indices come in any order after the key list (and
after `OP`). Run `msl_spike` with no arguments for the usage of every mode.

```
msl_spike synth OUT.bin SEED COUNT
```
Writes the made-up key list, the directed keys and then `COUNT` random ones from `SEED`.
`SEED` is a whole number from 0 to 4294967295 and `COUNT` one from 0 to 2147483647.

```
msl_spike emit KEYS.bin MSL_DIR
```
Writes `<n>.metal` for every key and `vertex.metal`, to read the MSL.

```
msl_spike compile KEYS.bin [cold|warm [ERRORS.txt [fast]]]
```
Compiles every key (library, then pipeline) and times it. `cold` adds a nonce to each program
so the OS cache can't serve it. Without it, the sources are compiled as they are, so a second
run is warm. Errors go to `ERRORS.txt`, or to stderr when it isn't given or can't be opened.
`fast` turns fast math on.

```
msl_spike parallel KEYS.bin
msl_spike glcompile KEYS.bin
msl_spike floor [HEADER_FILE...]
msl_spike blendvar KEYS.bin [fast]
```

- `parallel` compiles all keys at once through the async APIs.
- `glcompile` runs the same keys through GL (compile, link, first draw). With `NONONCE=1` in
  the environment it gives the warm figure. It prints how many programs failed to link and
  leaves them out of the times.
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
difference. It exits 0 if every key is within 1 level and its depth within 1e-6. A NaN depth
difference counts as past it. `logic` does the same over a filled target with GX logic op
`OP` (0 clear ... 7 or ... 15 set), through framebuffer fetch. On the made-up keys OR and XOR
pass only with `--fastmath`, for the reason under "What it proves". COPY passes either way.

The depth tolerance comes from the made-up keys. With the default inputs and with
`--indirect --zplane --screen`, the largest depth difference is 1.19e-07 with fast math off
and 5.96e-08 with it on. `--clipz` reaches 1.79e-07 and 1.19e-07, up to three steps of 2^-24.
1e-6 is about 17 steps of a 24-bit depth buffer (16777215 x 1e-6 is 16.8), so it passes a Z
texture or z bias error smaller than that. The `--flipfc` control moves a z freeze key's depth
by 0.587.

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
| `--fogmode=N` | the fog curve, GXSetFog's type (`u_fogmode`), from 0 to 15. The default is 2 (linear), 4 to 7 are the exponential curves and +8 is orthographic. 3 draws as 2 |
| `--fogA=F`, `--fogB=F`, `--fogC=F`, `--fogw=F` | set `u_fog`'s x, y, z and w (shadergen's A, C, bmag and bshift) to the float `F`; `inf` and `nan` work |
| `--linear` | bilinear sampling instead of nearest |
| `--clipz` | two vertices' z moved out of the near and far range (0.6 and -1.6), so the triangle is clipped |
| `--zprojw0`, `--zprojnan` | the fog's depth projection (`u_zproj`) with its w set to 0, or all of it 0. Depth reads it only on the VR paths |

Environment variables:

- `MATHMODE=N` sets `MTLMathMode` (0 safe, 1 relaxed, 2 fast) and overrides `--fastmath`. With
  `PRECISEFN=1` the floating-point functions are precise rather than fast.
- `NONONCE=1` leaves the nonce out of `glcompile`.

## Exit status

`msl_spike` exits 2 when the mode is unknown or has too few arguments. It also exits 2 when
a word of `compile` or `blendvar` isn't what the usage shows in its place (`cold` or `warm`
for `compile`, then `fast`), or when `synth`'s `SEED` or `COUNT` isn't a whole number in its
range. `compare` and `logic` exit 2 on an unknown option or an option value that isn't a
number in range, where `--fogmode` takes 0 to 15 and the four fog values take a float. They
do the same for an index that isn't a number or isn't in the list, an `OP` outside 0 to 15
and a run with no index at all. Each of these prints the usage or a one-line error naming
the argument. Arguments past the ones a mode takes are ignored.

Any mode exits 1 when a key list or other file can't be read, or a key list or MSL file can't
be written. The GL modes exit 1 when there's no GL context. Beyond that, these exit 1:

- `compile` when any key's library or pipeline fails
- `parallel` when any pipeline fails
- `glcompile` when any program fails to link
- `compare` and `logic` when any key is past 1 level in a channel or 1e-6 in depth

`synth`, `emit`, `floor` and `blendvar` don't check anything, so they exit 0 otherwise.

## Run everything

```
tools/msl_check/run_all.sh BUILD_DIR WORK_DIR GAME_KEYS.bin [ROUTE_KEYS.bin [N x10]]
```

Prints every measurement above in one log. `GAME_KEYS` is the whole list and `ROUTE_KEYS` a
smaller one for the ten-key and logic-op runs (the default is the same list). The ten indices
default to 0 to 9. `WORK_DIR` gets the MSL files, the error lists and the made-up keys.

The comparisons print their differences and the run goes on. Any other mode that fails stops
it. So does the negative control, where each of its keys has to fail on its own.

## Checking a shadergen change

A shadergen refactor should leave `gen_pixel_shader`'s and `gen_vertex_shader`'s output
unchanged byte for byte. Build `dump_glsl` against the old tree and this one, dump the same
key lists with each and `diff -r` the two directories. Each holds every key's `<n>.frag` and
`vertex.vert`, so a change meant to alter the output shows up as exactly the files it reached.
The steps run from the repository root and write their builds, key lists and dumps under
`build/`, which git ignores.

```
git worktree add --detach ../old OLD_REVISION
env SHADERGEN_ROOT=../old tools/msl_check/build.sh build/msl_old dump_glsl
tools/msl_check/build.sh build/msl_new dump_glsl
mkdir -p build/sg/old build/sg/new
build/msl_old/dump_glsl build/sg/old.bin build/sg/old - build/keys.bin
build/msl_new/dump_glsl build/sg/new.bin build/sg/new - build/keys.bin
diff -r build/sg/old build/sg/new
git worktree remove ../old
```
