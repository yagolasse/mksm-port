# mksm_recomp

An unofficial, experimental native Windows port of **Mortal Kombat: Shaolin Monks** (original
Xbox, title ID 4D570029). The game's own `default.xbe` is statically recompiled from x86 to C
with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), then built as a normal PC executable;
the Xbox's GPU is emulated by a port of [xemu](https://github.com/xemu-project/xemu)'s NV2A
renderer (OpenGL).

**This is not a playable release, and no game data is included.** You need your own copy of the
game disc. See [Legal](#legal).

## Status

Work in progress. Windows x64 is the only host tested so far.

| Area | State |
|---|---|
| Boot, intro movies (Sofdec), title, menus | working |
| Controller (SDL2 game controllers) and keyboard | working |
| Save games | working (stored under `%LOCALAPPDATA%\mksm_recomp`) |
| Audio | working; a stereo balance issue is open |
| First level | loads and plays at 60 fps; some rendering issues remain |
| Further levels, full playthrough | not tested yet |
| Internal resolution 1x-4x | working (`RECOMP_PB_GPU_SCALE`) |
| Widescreen | experimental (`RECOMP_ASPECT=16:9`, `RECOMP_ASPECT_WIDE=1`) |
| Settings menu (F1) | edits the settings (saved to `settings.ini`), Apply and restart, FPS counter, pauses the game; scale/aspect/wide apply on restart, the rest at once |
| Settings file | `settings.ini` read at start, monitor-based defaults on first start; written by the menu |
| First-start setup | asks for the disc image or game folder once, checks the version, extracts an image |
| Native Linux build | in progress: builds and runs on the Steam Deck (GPU renderer, window, pads, audio); movies don't open yet, no first-start setup, settings menu not checked by hand yet |

Expect crashes and glitches. Bug reports are welcome; please include the log (`../logs/` from the run
scripts, else `%LOCALAPPDATA%\mksm_recomp\last_run.log`; see [Run](#run)), never game files.

## What's included

- `src/main.c`: host entry point: loads the XBE, maps the Xbox memory layout, starts the
  recompiled game
- `src/recomp_manual.c`: hand-written overrides and hooks on top of the generated code (fixes,
  widescreen, diagnostics)
- `src/ui/`: the settings menu (Dear ImGui, `external/imgui`, over the GPU present)
- `src/gpu/`: the GPU renderer: xemu's PGRAPH and OpenGL back end (copied unmodified), the glue
  that stands in for the rest of the emulated GPU, and the present path to the game window
- `external/xboxrecomp/`: the recompiler toolkit and runtime (Xbox kernel, audio, input, D3D
  layer), a git submodule pinned to our fork; `patches/` holds our changes not yet upstream
- `tools/`: run scripts, the smoke test, a disc image extractor, seed and audit tools for the
  recompiler pipeline, profiling and audio analysis helpers, Ghidra scripts
- `seeds/`: extra function entry points for the disassembler (addresses only)
- `tests/`: unit tests for the Python tools, on synthetic data
- `docs/`: notes on how the port works (`gpu-renderer.md`)

The generated C (`src/recomp/gen/`) is **not** in the repository: it's derived from the game
binary, so you produce it yourself from your own copy (step 3 below).

## Requirements

- Windows 10/11 x64, a GPU with OpenGL 4.x
- Visual Studio 2022 (C++ workload) and CMake; Git (the first configure downloads SDL2's source
  and builds it into the exe)
- clang-cl (LLVM 18 or newer) and Ninja on `PATH`, for the GPU renderer library
- Python 3.10+ with `capstone` (`pip install capstone`)
- Git Bash (the run scripts and pipeline commands are POSIX shell)
- The game: an Xbox disc image (ISO) of Mortal Kombat: Shaolin Monks that you made from your own
  disc

## Quick start

1. **Clone** with the toolkit submodule:
   ```bash
   git clone --recursive <this repo> mksm-pc/port
   cd mksm-pc/port
   ```
   The submodule is our fork of xboxrecomp at a pinned commit. To rebuild it from upstream
   instead, check out upstream in `external/xboxrecomp` and apply `git am ../../patches/*.patch`.

2. **Extract your disc** next to the repository (the default game folder is `../game_files`):
   ```bash
   python tools/xiso.py extract /path/to/your.iso ../game_files
   ```

3. **Recompile the XBE** (from `external/xboxrecomp`; takes a while, writes `src/recomp/gen/`):
   ```bash
   cd external/xboxrecomp
   python -m tools.xbe_parser ../../../game_files/default.xbe --json ../../../game_files/default_analysis.json
   python -m tools.disasm ../../../game_files/default.xbe -v --seed-functions ../../seeds/observed.json \
       --seed-functions ../../seeds/padding.json --seed-functions ../../seeds/manual.json \
       --seed-functions ../../seeds/stubs.json --seed-functions ../../seeds/seh.json \
       --seed-functions ../../seeds/indirect.json
   python -m tools.func_id ../../../game_files/default.xbe -v
   python -m tools.abi_analysis ../../../game_files/default.xbe -v
   python -m tools.recomp ../../../game_files/default.xbe --all --split 250 --gen-dir ../../src/recomp/gen \
       --exclude-manual ../../src/recomp_manual.c
   cd ../..
   ```
   The optional Ghidra naming pass (`tools/ghidra/run_ghidra.sh`) only adds names to the
   generated code. If `tools.disasm` exits with a heap-corruption error, run it again (a known
   intermittent crash in capstone).

4. **Build**:
   ```bash
   cmake -S . -B build
   cmake --build build --config Release --parallel 6
   ```
   If clang-cl isn't at the default path, pass `-DMKSM_CLANG_CL=<path to clang-cl.exe>`.
   `-DMKSM_SDL_WINDOW=OFF` builds the older Win32 (GDI) game window instead of the SDL2 one.

5. **Check** (optional): `python tools/smoke_test.py` runs the game headless through the intros,
   the menus and into the first level.

## Run

```bash
tools/run.sh            # Git Bash; tools/run.bat from cmd
```

The scripts use `../game_files` as the game folder and write a log to
`../logs/run_<timestamp>.log`. The exe can also be started directly (double-clicked, or with
the game folder as its first argument; without one it uses `MKSM_GAME_DIR`, then the settings
file's `game_dir`, then `game\` in the folder it is started from or beside the exe): the
switches the game needs to
run (GPU path, game window, pads, sound) are built in,
and with nowhere to print (no redirect) it writes its log to
`%LOCALAPPDATA%\mksm_recomp\last_run.log` (the run before it: `last_run.old.log`).

**First start:** when none of those holds the game, the exe asks for it once. Pick either the
disc image (`.iso`; a trimmed xiso or a full disc dump) or `default.xbe` in a folder with the
extracted disc. The game's `default.xbe` must be the USA/Europe Xbox release (checked by its
SHA-256; another version is refused with a message). An image is extracted once, with a
progress window (about 4 GB, a minute or two; Cancel deletes the partial
copy), into `game\` beside the exe, so the whole install stays in one folder. If that folder
can't be written (e.g. under `Program Files`), has too little free space or already has a
`game\` without the game, it goes to `%LOCALAPPDATA%\mksm_recomp\game` instead. The folder is
then saved as `game_dir` in the settings file, so the next start goes straight in. To change it
later, edit or clear `game_dir` (cleared, an earlier extract in either place is found again).

### Keys and controllers

- **Controllers** (Xbox, PlayStation, Switch Pro and others SDL knows) work as pads 1-4 in the
  order they're found, and can be plugged in while the game runs.
- **Keyboard** (pad 1, needs the game window focused): arrows = d-pad, Enter = Start,
  Backspace = Back, Z/X/A/S = A/B/X/Y, Q/E = white/black, 1/3 = triggers, numpad 8/2/4/6 = left
  stick, I/J/K/L = right stick, Shift/Ctrl = stick clicks.
- **F1**, or **Back+Start** pressed together on a controller, opens the settings menu (Esc, F1,
  Back+Start or B closes it; arrows/Space or the d-pad/A move around it and change a value). It
  edits the settings file (below): resolution scale, aspect and wide picture are held as
  "pending restart" until **Apply and restart** (asks first: progress since the last save is
  lost; the game then starts again by itself) or **Revert**; the others (HUD at the edges,
  movie zoom, scaling filter, volume, fullscreen) take effect at once and are saved (the volume
  is heard once the menu is closed: the sound is paused while it is open). A setting given by an environment variable is shown as
  "set by ..." and can't be changed there. "Show FPS" toggles a counter. The game
  (and its sound, movies included) is paused while the menu is open and picks up where it was.
- **Alt+Enter** toggles borderless fullscreen; with a settings file the new state is saved, as
  the menu's Fullscreen box does.
- **F9** captures / releases the mouse when the mouse+keyboard pad is on (`RECOMP_MKB=1`). The
  mouse is captured only while the game window has focus and the settings menu is closed
  (Alt-Tab or F1 gives the pointer back). Mouse: left/right button = right/left trigger, wheel or
  middle button = Y, side buttons = white/black; keys: WASD = left stick, Space = A, E = B, R = X,
  Q/F = white/black, Enter/Esc = Start, Tab = Back, Shift/Ctrl = stick clicks, arrows = d-pad.

### Options

Options come from a settings file, `%LOCALAPPDATA%\mksm_recomp\settings.ini`, and from
environment variables; a variable that is set wins over the file. The run scripts and the smoke
test skip the file (`RECOMP_SETTINGS=0`, so a dev run looks like the Xbox): use
`RECOMP_SETTINGS=1 tools/run.sh` to run with it, or `RECOMP_SETTINGS=<path>` for another file.
Without the variable the exe uses the file, and writes it on its first start with defaults for
the main monitor: borderless fullscreen, internal scale = monitor height / 480 rounded (1080p →
2x, 1200 and 1440 → 3x, 2160 → 4x), the monitor's aspect with the wide frame, HUD at the edges,
movie zoom on. One `key=value` per line (`;` starts a comment, an empty value = the built-in
default). The settings menu (F1) writes it (keys it doesn't know are kept); it can also be
edited by hand while the game isn't running:

| Key | Variable it sets | Values |
|---|---|---|
| `scale` | `RECOMP_PB_GPU_SCALE` | 1-4 |
| `aspect` | `RECOMP_ASPECT` | `4:3` (off), `16:9`, `16:10`, `W:H` |
| `wide` | `RECOMP_ASPECT_WIDE` | 0/1 |
| `hud_edges` | `RECOMP_HUD_EDGES` | 0/1 |
| `movie_zoom` | `RECOMP_MOVIE_ZOOM` | 0/1 |
| `gauntlet_zoom` | `RECOMP_MOVIE_ZOOM_GAUNTLET` | 0/1 |
| `filter` | `RECOMP_PB_GPU_FILTER` | `linear`, `nearest` |
| `volume` | `RECOMP_APU_VOLUME` | 0..1 |
| `fullscreen` | `RECOMP_FB_FULLSCREEN` | 0/1 |
| `save_dir` | `MKSM_SAVE_DIR` | a folder for the saves (e.g. a cloud-synced one) |
| `game_dir` | `MKSM_GAME_DIR` | the game folder (written by the first-start setup) |

Saves (`UDATA`/`TDATA`) go to `%LOCALAPPDATA%\mksm_recomp\saves` unless `save_dir` /
`MKSM_SAVE_DIR` names another folder; the emulated hard disk and `settings.ini` stay in
`mksm_recomp`, so the saves folder can be synced alone. Saves of older builds (in `mksm_recomp`
itself or in the game folder) are moved into it once.

Set variables in front of the run script in Git Bash, or with `set` before `tools\run.bat` in
cmd:

```bash
RECOMP_PB_GPU_SCALE=2 RECOMP_ASPECT=16:9 RECOMP_ASPECT_WIDE=1 tools/run.sh
```
```bat
set RECOMP_PB_GPU_SCALE=2
tools\run.bat
```

| Variable | Default | What it does |
|---|---|---|
| **Display** | | |
| `RECOMP_FB_FULLSCREEN=1` | off | start in borderless fullscreen (Alt+Enter toggles it) |
| `RECOMP_WINDOW_SCALE=<n>` | 1 | open the window at n x 640x480 (up to 8; times the monitor's DPI scale) |
| `RECOMP_PB_GPU_SCALE=<n>` | 1 | internal resolution 1x-4x (sharper 3D; the 2D layer stays aligned as at 1x); read at start |
| `RECOMP_PB_GPU_FILTER=nearest` | `linear` | how the frame is scaled to the window: bilinear, or sharp pixels |
| **Widescreen** (experimental) | | |
| `RECOMP_ASPECT=16:9` | off (4:3) | wide 3D view; also `16:10`, `21:9`, `W/H`. Alone it stretches the 4:3 frame to that shape |
| `RECOMP_ASPECT_WIDE=1` | off | with `RECOMP_ASPECT`: render a really wider frame (square pixels); menus and text stay 4:3 in the middle |
| `RECOMP_HUD_EDGES=0` | on | with the wide frame: keep the in-game HUD at its 4:3 place instead of the screen edges |
| `RECOMP_MOVIE_ZOOM=0` | on | with `RECOMP_ASPECT`: don't zoom letterboxed movies to fill the width (zoom crops only their black bars). Without `RECOMP_ASPECT` movies look as on the Xbox |
| `RECOMP_MOVIE_ZOOM_GAUNTLET=1` | off | also zoom the Gauntlet trailer (cuts the top and bottom of its full-frame shots) |
| **Sound** | | |
| `RECOMP_APU_VOLUME=<0..1>` | 1 | output volume (0 = silent) |
| `RECOMP_AUDIO_BUF_COUNT=<n>` | 8 | audio buffers of ~10.7 ms; raise it if the sound crackles |
| **Input** | | |
| `RECOMP_KEYBOARD=0` | on | turn the keyboard pad off |
| `RECOMP_MKB=1` | off | mouse+keyboard pad on pad 1 (mouse = right stick); F9 captures the mouse |
| `RECOMP_MKB_SENS`, `_DZ`, `_ACC`, `_FULL`, `_INVERT_Y=1`, `_SWAP_STICKS=1` | | mouse sensitivity, dead-zone floor, acceleration, full-throw speed, inverted Y, mouse on the left stick. Also read from `mkb.ini` next to the exe (`sens=`, `dz=`, ...; `RECOMP_MKB_INI` names another file) |
| **Files** | | |
| `MKSM_GAME_DIR=<dir>` | `../game_files` | the extracted disc (the folder with `default.xbe`) |
| `MKSM_SAVE_DIR=<dir>` | `%LOCALAPPDATA%\mksm_recomp\saves` | where the saves go (the emulated hard disk stays in `mksm_recomp`) |
| `RECOMP_SETTINGS=0`, `=1`, `=<path>` | `0` in the run scripts, else the file | skip `settings.ini`, use it, or use another file |
| `MKSM_EXE`, `MKSM_LOG` | `build/Release/mksm_recomp.exe`, `../logs/run_<timestamp>.log` | run scripts only: which exe to run, where to log |

The switches the game needs to run (GPU path, game window, USB pads, audio) are on by default,
in the exe and the run scripts; leave those alone (`0` turns one off, e.g. `RECOMP_FB_WINDOW=0`
for a headless run). `CLAUDE.md` documents them, the pipeline and the debugging switches.

## Repository map

| Path | What |
|---|---|
| `src/` | host code, overrides, GPU renderer (`src/gpu/`) |
| `src/recomp/gen/` | generated C, made by you, ignored by git |
| `external/xboxrecomp/` | recompiler toolkit and runtime (submodule) |
| `patches/` | our toolkit changes as a patch series on upstream |
| `seeds/` | extra disassembler entry points |
| `tools/` | run scripts, smoke test, pipeline and analysis tools |
| `tests/` | unit tests (`python -m unittest discover -s tests`) |
| `docs/` | design notes |

## Legal

- **No game content is distributed here**: no disc image, XBE, extracted assets, movies,
  shaders, save data, disassembly, decompiled code or generated C from the game. The repository
  holds only original code, third-party open-source code (see below), and lists of addresses
  needed to process your own copy. Use a copy of the game you own; please don't attach game
  files or generated code to issues or pull requests, and don't ask where to get them.
- This project is not affiliated with, endorsed or sponsored by Warner Bros. Entertainment,
  Midway Games, NetherRealm Studios or Microsoft. *Mortal Kombat*, *Shaolin Monks* and related
  names are trademarks of their respective owners and are used here only to identify the game
  this software works with. Xbox is a trademark of Microsoft.
- The port's own code is licensed under the **GNU General Public License v3.0** (`LICENSE`).
  It includes third-party code under its own licenses (xemu's NV2A renderer and QEMU headers:
  LGPL-2.1-or-later, GPL-2.0-or-later, MIT, BSD-3-Clause; nv2a_vsh_cpu: Unlicense; xxHash:
  BSD-2-Clause; glad: generated code under WTFPL or CC0-1.0, Khronos definitions Apache-2.0;
  SDL2: zlib; Dear ImGui: MIT);
  `NOTICE` lists each component, its source and license, and GPL-3.0 is compatible with all of
  them for the combined work. The xboxrecomp toolkit is MIT-licensed, with its own notices in
  `external/xboxrecomp/`.
- The software is provided as is, without warranty of any kind (see `LICENSE`).

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet and contributors: the
  static recompiler and runtime this port is built on
- [xemu](https://github.com/xemu-project/xemu) and its contributors: the NV2A GPU emulation
- [OGmidway/mksm-recomp](https://github.com/OGmidway/mksm-recomp): the PS2 recompilation of the
  same game, a valuable cross-reference
- Midway's original developers, for the game
