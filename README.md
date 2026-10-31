# gcn-recomp

A toolkit for static recompilation of Nintendo GameCube games to native code: a Gekko
PowerPC → C recompiler, and a runtime that stands in for the GameCube hardware (CPU helpers,
OS context switching, graphics through an OpenGL GX pipeline, audio, DVD, controllers,
memory card) with an SDL desktop frontend.

It is the common layer under per-game projects, each of which supplies its own game's
layout and steering files and nothing else:

- [prime-recomp](https://github.com/ethannicholas/prime-recomp) — *Metroid Prime*
- [prime2-recomp](https://github.com/ethannicholas/prime2-recomp) — *Metroid Prime 2: Echoes*
- [bluestorm-recomp](https://github.com/ethannicholas/bluestorm-recomp) — *Wave Race: Blue
  Storm*, where this code was first written

**This repository contains no game code or data.** A game project builds from the user's
own disc image; the recompiled C lands in that project's build directory and nowhere else.

## Using it from a game project

Add this repository as a submodule and call `gcn_add_game()`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(prime C CXX)
add_subdirectory(gcn-recomp)
gcn_add_game(
  NAME prime
  TITLE "Metroid Prime"
  LABEL "Prime Recomp"
  DISC_ID GM8E01
  DOL_SHA1 39a2f928159ad46491e9b1ef0a72613d91e0cc40
  SYMBOLS ${CMAKE_SOURCE_DIR}/analysis/symbols.txt
  TABLES ${CMAKE_SOURCE_DIR}/recomp
  FPS 60)
```

That defines the game executable, a headless benchmark (`<name>_bench`) and a compile-only
`runtime_check` target. With `ANDROID_PACKAGE <id>`, an Android build (the NDK's toolchain
file, `-DGCN_BENCH_ONLY=ON`) also builds the headset app, `lib<name>.so`, which
`tools/package-apk.ps1` turns into an APK, and `<name>_egl`, the renderer headless on a
device. A game tells those what it knows -- when to show stereo, how big its world is --
through `vr::GameHooks` (`runtime/vr_game.h`) in its `SOURCES`.

TITLE is the game's name, used in messages. LABEL, optional, is what the window and the
headset app are called, so the app can carry a name of the project's own rather than the
game's trademark.

The game project provides:

- `analysis/symbols.txt` — the DOL's function layout in decomp-toolkit's format, either
  from `dtk dol split` (`tools/fetch_dtk.sh` downloads it) or from a decompilation project.
- `recomp/hle.txt` — functions whose body the runtime replaces (`OSSaveContext`,
  `OSLoadContext`, `OSReport`, `OSPanic` and `OSResetSystem` at minimum).
- `recomp/special_calls.txt` — the address of `OSSaveContext`, whose call sites become
  resume points. A game whose code uses the C library's `setjmp` and `longjmp` lists them
  too, by those names, so that a `longjmp` comes back where its `setjmp` was called.
- `recomp/idle.txt` — the backward branch of the SDK scheduler's idle spin (`SelectThread`
  waiting for a runnable thread), where the runtime skips guest time to the next event.
- `recomp/names.txt`, `recomp/patches.txt` — hand-named functions and instruction patches,
  usually empty.
- `recomp/steps.txt` — the game's per-frame steps, for running it at another frame rate
  (`docs/diagnostics.md`, "Finding a game's per-frame steps"); absent for a game that
  does not try.

## The window

The window opens 960 points high (1920 pixels on a display of two pixels to a point) and
as wide as the output aspect makes it, or smaller in the same shape where the display's
usable area is smaller, as for 21:9 on many displays. Its title bar and frame fit in that
area too. The default aspect is 4:3, the console's own. `--aspect=W:H` stretches the
picture to another, such as 16:9 or 21:9, with W and H whole numbers from 1 to 9999.
`--aspect=window` follows the window's own shape as it's resized. A window of another shape
shows the picture centred between black bars. The window as it opens has none for the usual
shapes. An odd ratio on a two pixel display, or a portrait shape, can leave a bar a pixel
wide. A game's hooks read the aspect with `gx::output_aspect()`, so it can draw its world
wider to match. A headset's view stays 4:3, so `--eye` and `GCN_THEATER_STEREO` ignore
`--aspect`. `--fullscreen` opens it fullscreen. F11 switches it on Windows and Linux, and
Control-Command-F on a Mac, from the Window menu SDL adds, where F11 is Show Desktop by
default.

F1 or a gamepad's Back opens a menu over the game and pauses it until F1, Back or B closes it.
A gamepad with no Back, as SDL maps a GameCube controller, opens and closes it with L, R and
Start held together, L and R clicked all the way in. Escape closes the menu too, and quits the
game when the menu is closed.

On a gamepad A picks and B goes back, the buttons the game takes as A and B, so a Switch Pro's
are the other way round from an Xbox pad's.

The menu's Display page sets the aspect and the internal scale and turns fullscreen on or off.
Each change applies at once and is saved to `settings.txt`, which the window reads at start.

Restart game and Quit game sit below every page, and each asks first, with Cancel picked.
Cancel, B or Escape goes back with nothing done. A replay has no Restart game, since its log
can't hold one.

Quit ends the run as Escape does with the menu closed, saving the memory card on the way out.
Either way, a save the game is part way through gets up to 3 seconds to finish first. Restart
ends the run the same way, then starts the game again as the game's own reset does.

## Audio

This fork changes the audio path from upstream `9b0a2fe` in the commits below. Nothing in it
keys on a game. The mixer control layout, the compressor and the PB's length follow the AX
ucode's hash.

| Change | Commits |
|---|---|
| The PB's mixer control word decodes with the 2001 layout for ucode `4E8A8B21` only. Every other build gets the later layout, with an enable bit for each of the nine buses and ramps by group. `GCN_AX_LAYOUT` forces either one | `6616d55`, `3bde515` |
| Command `12`, the compressor, runs on every ucode build but `4E8A8B21`, which has no such command | `bf31b7b`, `b839cf6` |
| On every build but `4E8A8B21` the PB is 97 words long, and the mixer reads, writes back and updates its four low-pass filter words. It doesn't apply the filter | `3056588` |
| Source type 0 resamples through a four-tap polyphase filter of 128 phases, in one of three sets that PB word 5 picks. The ucode reads its sets from the DSP's ROM, which the runtime doesn't have, so the runtime computes its own. A set past 2 interpolates linearly | `cce9fbc`, `795ccc4` |
| Source type 2 and every type past it take one sample in for each sample out, with no rate conversion | `fd1f6a8`, `b839cf6` |
| Each bus a voice mixes into leaves the voice's last sample in the PB's depop words, which the AX library fades out through the setup command when it stops the voice. In a mode whose lists send `07` or `11` only the aux buses' fades reach the output | `3b025ac` |
| A bus the setup command starts at zero stays silent whatever its slope | `84471cc` |
| A mixed sample, command `0E`'s output and an ADPCM sample the accelerator decodes clamp at `-32768`, where they stopped at `-32767`. The volume envelope still clamps at `-32767` | `2b7ae75`, `3d4fe9b`, `6def94c` |
| The AI DMA's quarter block hold, for a block the guest hasn't queued after, now counts only the guest's own running and leaves out the clock's catch-up jumps. The hold is looked at again every 32nd of a block and ends once the guest has queued | `a87ab06`, `3c55458` |
| The sound ring crossfades a cut over 10 ms, and a cut with the ring over half full splices. Near empty it fades out over 5 ms, and it fades back in over 8 ms | `3ae8463`, `dbd8c99` |
| While a pause holds the guest, DVD streamed music fades out over 5 ms and keeps its place on the disc. On the resume it fades back in over 8 ms from the next sample | `26db0da`, `c0a12ea` |
| The desktop frontend plays through an SDL3 audio stream and asks the device for 512 frames a pull | `d9ee73d` |
| Under SDL's dummy audio driver the runtime pulls 512 frames itself on 48 kHz deadlines, so a run with no sound keeps a sound card's pace | `e6ea431`, `dbd8c99` |
| A voice loops or stops only on reading the sample at its end address, as the DSP does. A voice that loops above its old end plays on from the loop, where it used to sit on the loop's first sample until the end moved. An ADPCM end on a frame header wraps the voice to its loop, as on the DSP | `1f5e5a3`, `ac8a1f4` |
| `GCN_AXVOICES` prints what the game and the mixer do to each voice. Every 200 lists it also gives each voice's loudest sample | `1b25342` |
| `GCN_AXSTATS` also prints command counts, bus peaks, the samples the output clamp changed, what the running voices ask for, a hash of the output and each ring cut | `8fabc65`, `3056588`, `3f5da1e` |
| `GCN_AUDIOSTATS` prints the device it opened and each late callback and underrun. It also says where the device's output lines up with the DMA's stream | `3c71be8` |
| `ninja test_ax`, `test_aid`, `test_clock` and `test_audio_ring` check the mixer, the DMA's hold, the clock's jumps and the sound ring with no disc or device. `test_audio_ring` also checks the held DVD music and the `GCN_AUDIOSTATS` accounting | `3056588`, `a87ab06`, `250ebda`, `3ae8463`, `3c71be8`, `6e361ba`, `26db0da`, `c0a12ea`, `244fa9e`, `84698f6`, `b0dcc2e` |

The sound of a GameCube racing game on a later AX build was measured by program, with nobody
listening. The method, the routes and every figure below are in
[the game project's scorecard](https://github.com/lunchbox-recomp/dragoon/blob/ffa002217b804735ae5bf9c644ec226c04f5ce4a/docs/dev/scorecard.md).

Passes 1 to 4 and the title at 91 s compare `caf12a2` (before) with `b848158` (after), and
pass 5 compares `b848158` with `b97c019`. `caf12a2` is upstream plus 13 commits whose only
audio change is the SDL3 port. Pass 4 played only before and pass 3 only after.

The mixer stage is every block the DMA delivered, and the device stage is what the ring
handed the device. Unpaced figures cover the five routes' 324 scored seconds.

An unpaced run's guest clock doesn't wait on the host, and each pair of unpaced runs compared
gave the same samples. A real-time run's clock jumps to catch up when the host falls behind,
so two real-time runs of a route needn't sound the same. Each real-time cell gives each
run's figure in order.

| Measured | Passes | Before | After |
|---|---|---|---|
| Mixer clicks, five routes unpaced | 4 and 3 | 132 | 136 |
| Mixer gaps, five routes unpaced | 4 and 3 | 3 | 8 |
| Mixer clipped runs (samples), five routes unpaced | 4 and 3 | 208 (2,200) | 25 (371) |
| Highest main L peak before the clamp, five routes unpaced | 4 and 3 | 85,094 | 64,999 |
| Mixer clicks, title played to 91 s in real time | Title at 91 s | 1,097, 979 | 22, 22 |
| Device clicks a minute, five routes in real time | 1 and 2 | 57.9, 45.1 | 36.3, 33.1 |
| Mixer clicks, five routes unpaced | 5 | 136 | 94 |
| Mixer clicks on a block join, five routes unpaced | 5 | 35, against 4.2 by chance | 7, against 2.9 by chance |

Over the scored seconds the mixer stage's clicks didn't fall at `b848158`, and what fell
there was clipping. On the title and the mode select the clicks fell past the seconds the
totals score. A bisect in the scorecard puts the five new gaps in an intro stream that was
digital silence before `fd1f6a8`.

In pass 5 every route made fewer mixer clicks at `b97c019`, and the scorecard doesn't say
which commit took them away. Pass 6 played `3a8d154` and `dac449e` four times each, and
their unpaced mixer stage scored the same as pass 5's `b97c019` on every figure. Their
real-time mixer and device clicks overlapped across the plays.

In passes 1 and 2 the before build's device stage ran at the pace of SDL's dummy driver,
which took 92.7% and 92.8% of the sound the DMA delivered. That build doesn't log the
ring's cuts, so its log doesn't say where the rest went.

A click is any sharp edge, the game's own sounds included. Nobody listened to any build, and
nothing was compared with a real console. Every run was on one machine.

## The disc drive

A DVD read takes the time a drive would take over it, where upstream gave every read 100 us
and 64 MB/s. At that rate a music stream could start in one audio frame, so the AX library
dropped its loop flag and the music stopped after its first buffer.

By default every block reads as one the drive has read ahead, so a read costs 600 us and its
length at 32 MiB/s, as Dolphin's "Speed up disc transfer rate" has it (`22bc2c8`, `7968abc`).
`disc console` in `settings.txt` or `--disc=console` times reads as a console's drive does,
with Dolphin's model and numbers, measured on hardware (`04c025f`). On one menu route that
made two scene changes freeze for 1.5 s each, against 0.4 s and 0.2 s at the default.

A replay reads at the speed its input log was recorded at (`80e1d38`), and `session.json`
names the speed (`6fe5b37`). [The diagnostics](docs/diagnostics.md#the-disc-drive) give
both speeds' numbers.

A read's DIMAR and DILENGTH move when it completes, since a read at console speed can take
hundreds of milliseconds (`f19f98d`).

## Layout

- `recomp/`: `recomp.py` drives the translation; `ppc.py` turns each instruction into C;
  `dol.py` reads DOLs, `.iso` and `.ciso` images and symbol files; `extract_dol.py` pulls
  and verifies the DOL.
- `runtime/`: the GameCube in software. `recomp.h` is the contract with generated code.
  `cpu.cpp`, `threads.cpp` and `hle_os.cpp` are the CPU and OS; `hw/` the hardware
  registers; `gx/` the graphics pipeline; `main.cpp` the SDL frontend and `bench_main.cpp`
  the benchmark. `gamepads.*` gives each gamepad a port of its own as it connects. It
  takes its saved port if that's free. A gamepad the file doesn't name takes the port of
  its player number if that's free (Steam Input's order, or the XInput slot on Windows),
  else the lowest free port, and a saved one that finds its port taken takes the lowest
  free port too. Its player light then shows that port, except an XInput controller's
  ring, which shows the slot Windows gave it.
  A gamepad pulled out frees its port, which the game then sees unplugged unless the
  keyboard (port 1), `--pads` or a `controls.txt` line keeps it plugged in. A gamepad gets
  no port when none is free, and gets one only after it is unplugged and plugged in again.
  A gamepad plugged back in takes its saved port if that's free, so two gamepads pulled
  out and plugged back in the other order keep their ports. A controller SDL has no
  mapping for is a gamepad once a `gamecontrollerdb.txt` beside the executable maps it; for
  a file elsewhere, SDL reads the path in `SDL_GAMECONTROLLERCONFIG_FILE`.
  `input_map.*` binds any joystick or key to a port from a `controls.txt` (wheels, pedals,
  button boxes; port 1 unless a line says `port <n>`), and its `--list-controls` prints
  what to write for each. A port a device's `port <n>` line names, `port 1` too, is kept
  from gamepads, and the first such device present shows the port on its player light;
  `ffb.*` is the wheel's force feedback, from the game's `telemetry::Motion` and the pad's
  rumble motor.
- `runtime/port_memory.*`: `saves/ports.txt`, one line a port, naming the gamepad last put
  there by its GUID, serial and USB IDs. A saved port is only a preference. Another gamepad
  can take it while its own is away. The file is written only when a gamepad lands on a
  port the file didn't name it on.
- `runtime/settings.*`: `settings.txt` in the working directory, one `key value` a line with
  `#` comments, which the desktop frontend reads at start. The runtime owns `aspect`,
  `fullscreen`, `scale` and `disc`. A flag beats the file for that run and isn't written
  back, and a save rewrites only the owned keys' lines and adds any that are missing.
- `runtime/overlay/`: the menu over the game, drawn with Dear ImGui. It pauses the game and
  holds the game's input back while it's open. Its Display page sets the picture and saves
  what it changes to `settings.txt`. Its Restart calls `frontend_restart` in `main.cpp`, as a
  game's own page can.
- `openxr/`: the OpenXR frontend (the game on a theater panel or in stereo, the headset's
  controllers as the pad). `xr_frontend.cpp` is the core every platform shares;
  `xr_desktop.cpp` puts it on the desktop's GL context for PC VR (`--vr`, Windows).
- `android/`: the standalone headset's shells. `openxr_main.cpp` puts the OpenXR frontend on
  EGL and the NativeActivity, `egl_main.cpp` is the headless harness and `audio_aaudio.cpp`
  the audio device. The `vr.txt` settings
  (`runtime/vr_config.*`) and what a game tells the headset (`runtime/vr_game.h`) are in the
  runtime, so the desktop can check a game's hooks too.
- `tools/package-apk.ps1`: packages the app into an APK without Gradle, and installs it.
- `docs/`: working notes on diagnostics and the renderer.

## Requirements

CMake 3.20+, Ninja, Python 3 and a C++20 compiler. The first configure downloads SDL
3.4.18's source release, checks its SHA-256 and builds it static into the build directory.
`-DGCN_SYSTEM_SDL=ON` uses an installed SDL3 (3.2 or later) instead. If libusb's shared
library is installed when SDL is configured, SDL also reaches controllers over libusb,
loading libusb at run time rather than linking it. On macOS that is
`brew install cmake ninja python`, plus `libusb`.

The menu over the game needs Dear ImGui 1.92.9b, whose source archive the first configure
downloads and checks by its SHA-256. `-DGCN_OVERLAY=OFF` builds without the menu, and Android
builds and the benchmark leave it out.
