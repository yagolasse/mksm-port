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
| Controller (XInput) and keyboard | working |
| Save games | working (stored under `%LOCALAPPDATA%\mksm_recomp`) |
| Audio | working; a stereo balance issue is open |
| First level | loads and plays at 60 fps; some rendering issues remain |
| Further levels, full playthrough | not tested yet |
| Internal resolution 1x-4x | working (`RECOMP_PB_GPU_SCALE`) |
| Widescreen | experimental (`RECOMP_ASPECT=16:9`, `RECOMP_ASPECT_WIDE=1`) |
| Native Linux build | started, paused |

Expect crashes and glitches. Bug reports are welcome; please include the log from `../logs/`
(see [Run](#run)), never game files.

## What's included

- `src/main.c`: host entry point: loads the XBE, maps the Xbox memory layout, starts the
  recompiled game
- `src/recomp_manual.c`: hand-written overrides and hooks on top of the generated code (fixes,
  widescreen, diagnostics)
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
- Visual Studio 2022 (C++ workload) and CMake
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

5. **Check** (optional): `python tools/smoke_test.py` runs the game headless through the intros,
   the menus and into the first level.

## Run

```bash
tools/run.sh            # Git Bash; tools/run.bat from cmd
```

The scripts set the needed defaults, use `../game_files` as the game folder (`MKSM_GAME_DIR`
overrides it) and write a log to `../logs/run_<timestamp>.log`.

- Keyboard: arrows = d-pad, Enter = Start, Backspace = Back, Z/X/A/S = A/B/X/Y, Q/E =
  white/black, 1/3 = triggers, numpad 8/2/4/6 = left stick, I/J/K/L = right stick. An XInput
  controller works as pad 1.
- Alt+Enter toggles borderless fullscreen (`RECOMP_FB_FULLSCREEN=1` starts in it).
- `RECOMP_PB_GPU_SCALE=2` (or 3, 4) renders at a higher internal resolution.
- `RECOMP_ASPECT=16:9 RECOMP_ASPECT_WIDE=1` (experimental) renders a 16:9 view with the 2D
  layer pillarboxed; movies stay 4:3.

`CLAUDE.md` documents every option, the pipeline and the debugging switches in detail.

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
  BSD-2-Clause; glad: generated code under WTFPL or CC0-1.0, Khronos definitions Apache-2.0);
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
