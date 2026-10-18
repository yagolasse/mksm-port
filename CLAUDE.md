# mksm_recomp

Native Windows port of Mortal Kombat: Shaolin Monks (original Xbox, title ID 4D570029) built by
statically recompiling the retail `default.xbe` to C with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp). Users supply their own game disc.

**This repo is intended to become public.** Keep it free of copyrighted or game-derived material.

## Hard rules
- Never commit game data: `.xbe`, `.iso`, extracted assets, WAD contents, movies, shaders from the disc.
- Never commit `src/recomp/gen/`. It's generated from the game binary (gitignored).
- Don't paste disassembly or decompiled game code into this repo. Hand-written reimplementations
  and overrides in `src/` are fine.
- Private research notes belong in the separate `../docs` repo, not here.
- `README.md` is the user-facing doc: whenever a change adds or changes something a user can set
  (an env var, a flag, a key, a run-script option, later a settings-menu entry), update its
  Run section (and the Status table if a feature's state changed) in the same commit.
  Debug-only switches (logs, A/B toggles) stay in this file only.

## Layout
- `CMakeLists.txt`: game executable `mksm_recomp`; pulls in the toolkit via `add_subdirectory`
- `src/main.c`: host entry: loads the XBE, maps Xbox memory, inits kernel/bridge, calls the guest entry `0x000F40CA`
- `src/prof.c`: `RECOMP_PROF` sampling profiler (all threads, symbolized from the PDB; see Debugging)
- `src/settings.c`: the player settings file (`settings.ini`, plan B7b1; see Saves and settings):
  INI parse/write, monitor defaults, exports the keys as the env vars they stand for. Its C unit
  test is `tests/settings_test.c` (CTest `settings`; `tests/test_settings.py` runs the built exe)
- `src/recomp_manual.c`: hand-written overrides (`recomp_lookup_manual`), entry hooks
  (`sub_X_enter`, see Pipeline), widescreen (`RECOMP_ASPECT`) and ICALL diagnostics
- `src/recomp/gen/`: generated C (not in git)
- `src/gpu/`: GPU renderer library `mksm_gpu` (xemu's PGRAPH; the game uses it with `RECOMP_PB_GPU=1`): its own
  CMake project built with clang-cl (see Build). `xemu/hw/xbox/nv2a/` = xemu files **copied unmodified**
  (same paths as in xemu, so they diff cleanly; never edit them, fix things in `xemu/shim/` or the glue);
  `xemu/include/`, `xemu/util/` = QEMU headers/sources from xemu's tree that the GL renderer uses
  (`qemu/lru.h`, `queue.h`, `mstring.h`, `fast-hash`), also unmodified;
  `xemu/shim/` = stand-ins for the QEMU/glib/xemu headers they include (incl. `epoxy/gl.h` → glad);
  `nv2a_vsh_cpu/`, `xxhash/` = vendored libraries; `glad/` = generated GL 4.6 core/WGL loader;
  `gloffscreen_sdl.c` = xemu's GL context layer on SDL2 (hidden SDL GL windows, newest 4.x core, shared
  contexts; plan B9a2, the default), `gloffscreen_wgl.c` = the same on WGL (only with
  `-DMKSM_SDL_WINDOW=OFF`); `gloffscreen_window.h`: their context on the game window for the present;
  `xemu_stubs.c` = externals from unported xemu files; `shader_patch.c` = edits to xemu's generated GLSL
  (hooked at `glShaderSource`); `mksm_gpu.c` = stands in for xemu's PFIFO
  (RAMHT lookup, acks the NOP-notify and flip-stall waits, presents the last-drawn surface at the
  flip and paces flips to 59.94 Hz); `mksm_gpu.h` = the plain-C interface for
  cl-built code. Licenses: `LICENSE` (GPL-3.0, the combined work), `LICENSES/`, `NOTICE` (file → license)
- `src/ui/`: the settings menu (`menu.cpp`, plan 4.6/B7a1, editable since B7b2; C++, cl-built lib `mksm_menu`, linked
  with the SDL window + GPU in non-Debug configs, `MKSM_HAVE_MENU`): Dear ImGui with its SDL2 and
  OpenGL3 backends, drawn by `mksm_gpu.c`'s present thread through `mksm_gpu_set_overlay`
- `external/imgui/`: Dear ImGui 1.91.9b, the core files and the sdl2/opengl3 backends copied
  **unmodified** (`NOTICE`); update by copying a newer release over them
- `external/xboxrecomp/`: toolkit git submodule (pinned), from our fork
  https://github.com/yagolasse/xboxrecomp (remote `origin`); add sp00nznet's repo as remote
  `upstream` after cloning (`git remote add upstream https://github.com/sp00nznet/xboxrecomp.git`):
  rebases and `tools/regen_patches.sh` use `upstream/main`. Prefer fixing things in our `src/`
  first; toolkit changes stay in the fork and go upstream once the port is done
- `tools/run.sh`, `tools/run.bat`: run the exe with the needed env defaults, log to `../logs/`
- `tools/smoke_test.py`: boot smoke test (also CTest `smoke`), see Testing
- `tools/xiso.py`: list/extract files from an Xbox disc image
- `tools/ghidra/`: `run_ghidra.sh` (headless import + name export, see Pipeline),
  `RefsTo.java` (the functions that reference given addresses) and `DecompAt.java` (decompile the functions at given addresses into a file, read-only:
  `analyzeHeadless.bat <ghidra/project> mksm -process xbe_flat.bin -noanalysis -readOnly
  -scriptPath <dir> -postScript DecompAt.java <out.c> 0x0011D140 ...`; copy it next to the other
  scripts in `../ghidra/scripts` first). Output is game-derived: it goes to `../docs/raw/`
- `tools/padding_seeds.py`: finds undetected functions after int3 padding (writes `seeds/padding.json`)
- `tools/seh_seeds.py`: the filter/handler blocks of the XBE's SEH scope tables (writes `seeds/seh.json`)
- `tools/wavscan.py`: scans a `RECOMP_AUDIO_WAV` recording (older logs: `RECOMP_APU_WAV`) for noise bursts, clicks, repeated/dropped blocks, clipping; `--excerpt START,DUR out.wav` cuts a clip
- `tools/voicedump.py`: reads a `RECOMP_APU_VOICE_DUMP` (per-voice samples): summary, stale-ring repeats, one voice as WAV. Two formats: our old record dump (with buffer offsets), and toolkit v0.14's raw s16 stereo 48 kHz files (`.raw`, one per voice, `=all` writes `<file>.<voice>.raw`): level, zero-crossing rate, exact copies of earlier output
- `tools/profsum.py`: summarizes `RECOMP_PROF` run logs: per-thread CPU % + top functions, level fps per `[XGPU]` interval, medians over several logs (A/B runs)
- `tools/stub_audit.py`: sorts `gen/recomp_stubs_unresolved.c` into real code vs. decoded data (writes `seeds/stubs.json`)
- `docs/`: generic notes on how the port works (`gpu-renderer.md`: design of the GPU renderer, a port of xemu's PGRAPH + OpenGL backend)
- `tests/`: unit tests for `tools/*.py` on synthetic inputs (no game data), see Testing
- `seeds/`: extra function entry points for `tools.disasm --seed-functions` (see Pipeline)
- `patches/`: our toolkit changes not yet upstream; apply them in `external/xboxrecomp` after cloning
  (`git am ../../patches/*.patch`). Regenerate them with `bash tools/regen_patches.sh [slug ...]`
  (one slug per new submodule commit): it writes them from git, then checks that the series applies
  on upstream and reproduces the pin. Never write them with PowerShell's `>`: it saves UTF-16 with
  CRLF, which `git am` rejects ("Patch format detection failed")

## Pipeline (run from `external/xboxrecomp`, game data in `../../../game_files`)
```bash
python -m tools.xbe_parser ../../../game_files/default.xbe --json ../../../game_files/default_analysis.json
python -m tools.disasm ../../../game_files/default.xbe -v --seed-functions ../../seeds/observed.json \
    --seed-functions ../../seeds/padding.json --seed-functions ../../seeds/manual.json \
    --seed-functions ../../seeds/stubs.json --seed-functions ../../seeds/seh.json \
    --seed-functions ../../seeds/indirect.json  # all code sections
python -m tools.func_id ../../../game_files/default.xbe -v
python -m tools.abi_analysis ../../../game_files/default.xbe -v
bash ../../tools/ghidra/run_ghidra.sh   # our wrapper (Java export; Ghidra 12 has no Jython), ~20+ min
python tools/ghidra_naming/merge_names.py --apply
python -m tools.recomp ../../../game_files/default.xbe --all --split 250 --gen-dir ../../src/recomp/gen \
    --exclude-manual ../../src/recomp_manual.c
```
- `--exclude-manual` (since plan B5a) scans `src/recomp_manual.c`: a `void sub_X_enter(void)` there
  is an entry hook (the generated `sub_X` calls it first, on every path, direct calls included);
  a `void sub_X(void)` there replaces the generated body (`extern void sub_X_gen(void);` keeps it
  under that name to wrap). `recomp_lookup_manual` only sees indirect calls and thread/APC/DPC
  routines: the lifted code calls direct targets by symbol. A lift without the flag fails to link
  once a hook exists (or, worse, links without them if the hooks were removed), so keep it.
- Use all code sections. The game calls directly into its statically linked D3D/DSOUND/D3DX/WMADEC/XPP/
  PSFD00 (Sofdec)/DOLBY sections.
- Tool outputs land in `external/xboxrecomp/tools/*/output/` (gitignored there).
- `seeds/`: function entries the detector misses (only addresses, no code).
  `observed.json` comes from runs: after a crash with `[ICALL] Failed to resolve VA`, run
  `python -m tools.seed_from_log <run.log> ../../../game_files/default.xbe --functions tools/disasm/output/functions.json --seeds ../../seeds/observed.json`,
  drop entries in the incremental-link thunk range `0x00011005..0x00016CDA` (handled at runtime),
  then re-run disasm and everything after it. `padding.json` is from `tools/padding_seeds.py`
  (functions after int3 padding that nothing calls; regenerate only from an unseeded disasm).
  `manual.json`: hand-found entries, each with a note saying why (e.g. a jump target after zero
  padding). `seh.json` is from `tools/seh_seeds.py` (the filter/handler blocks of MSVC SEH scope
  tables: only the CRT's exception handler and `RtlUnwind` reach them, through the table, so the
  stub audit can't see them missing; toolkit v0.14's disasm dropped 41 of the 146). It depends
  on the XBE only, so regenerate it from any disasm. Since v0.14 a seed inside a detected
  function becomes an alias entry and doesn't split that function.
  `indirect.json` is from `tools/indirect_seeds.py`: every incremental-link thunk target (the
  runtime follows a thunk, but its target must be lifted; v0.14's disasm dropped 27, e.g.
  `0x00036740`, `0x0006AC30`); XBE-only, regenerate any time. Don't seed an address just because
  a data word equals it or an older lift had an entry there: small data values look like
  pointers, and a seed in the middle of a function cuts it, so a loop over the cut recurses
  until the host stack overflows (`0xC00000FD` at boot, plan H.5b1). Those go through the crash
  loop (`observed.json`). After a toolkit update, compare the new lift's dispatch table with the
  old one: entries it drops that nothing calls directly don't show up in the stub audit. A call or jump to an undetected address lifts to a silent no-op stub in
  `gen/recomp_stubs_unresolved.c` (`/* not detected */`); a `jmp`/`jcc` to one makes the jumping
  function return early. After each lift run `tools/stub_audit.py`: its `entry`/`split` rows are
  real code and go to `stubs.json`. `--seeds` overwrites the file with the current rows only, so
  once a lift has used `stubs.json`, write to a temp file and merge the new rows in. The other
  classes are only referenced from decoded data (jump tables, strings) or dead code.
  Each seeded alias can jump back into its parent and create new `split` rows, so repeat
  lift → audit → merge until the audit has no `entry`/`split` rows (1.12c took 5 + 2 rounds).
- Switches the lifter doesn't decode (a byte index table feeding a jump table) lift as
  `RECOMP_ITAIL(MEM32(reg * 4 + table))`; every table target must be a function or alias, or the
  call fails at runtime with `Failed to resolve VA`. Those targets are in `manual.json`. To find
  more: `grep -hoE "RECOMP_ITAIL\(MEM32\([a-z]+ \* 4 \+ 0x[0-9A-F]+\)\)" src/recomp/gen/*.c`.
- `tools.disasm` sometimes dies in capstone during the linear sweep (heap corruption 0xC0000374,
  shows as exit 139 in Git Bash). It's not deterministic; just run it again.

## Build
VS 2022 + CMake, x64 (the default; don't force `-A Win32`).
```bash
cmake -S . -B build
cmake --build build --config Release --parallel 6
```
Use `--parallel N`, not `-- /m`: Git Bash rewrites `/m` into a path (`M:/`) and MSBuild fails.
`src/gpu` (xemu's renderer, GNU C that `cl` rejects) is built as an ExternalProject with clang-cl + Ninja
into `build/mksm_gpu/mksm_gpu.lib` (always Release, `/MD`) and imported as target `mksm_gpu`. It needs
`MKSM_CLANG_CL` (cache var, default `C:/clang+llvm-18.1.8-x86_64-pc-windows-msvc/bin/clang-cl.exe`; that
folder actually holds clang 22.1.8) and `ninja` on PATH, else it is skipped with a status message.
The game links it in non-Debug configs only (`MKSM_HAVE_GPU`; the lib is `/MD`). Link it with `$<LINK_LIBRARY:WHOLE_ARCHIVE,mksm_gpu>`: the null renderer registers from a constructor in
an object nothing references, and a plain link drops it ("No available renderer", then a crash).
## Run
The exe takes the extracted game folder (holding `default.xbe`) from its first argument, else
`MKSM_GAME_DIR`, else `game\` in the working directory (`resolve_game_dir` in `src/main.c`).
It's a GUI-subsystem exe, so stdout only appears when redirected. Use a run script, which
defaults to `../game_files` and logs to `../logs/run_<timestamp>.log`, and turns on the GPU path and the
game window (`RECOMP_PB_GPU=1`, `RECOMP_FB_WINDOW=1`; `=0` turns either off):
```bash
tools/run.sh                                  # MKSM_GAME_DIR / MKSM_EXE / MKSM_LOG override
RECOMP_WATCHDOG_SECS=10 timeout 60 tools/run.sh
```
The window is SDL2's since plan B9a1 (toolkit `fb_present_sdl.c` with `XBOX_SDL_WINDOW`, on its
own `fb window` thread; SDL 2.32.10 fetched by CMake and linked static, `build/_deps/`); CMake
`-DMKSM_SDL_WINDOW=OFF` builds the older GDI window (`fb_present.c`, patch 0014) with WGL contexts for A/B runs.
Both scale the frame to fit with black bars and can be resized; Alt+Enter toggles borderless
fullscreen (not seen by the game as Start), `RECOMP_FB_FULLSCREEN=1` starts fullscreen,
`RECOMP_WINDOW_SCALE=<n>` (SDL window) opens it at n x 640x480 (default 1, times the monitor's
DPI scale). On the GPU path `main.c` puts the SDL window in external-present mode
(`xbox_FramebufferWindowExternalPresent`): no SDL_Renderer on it, made with `SDL_WINDOW_OPENGL`
(patch 0021), so the GPU present's SDL GL context owns it (B9a2; `xbox_FramebufferWindowSdl`), and
the CPU frames before the present thread is up are dropped (black for a moment at boot). Settings menu (B7a1, SDL window + window present only): F1 or Back+Start held together
toggles it, Esc / B closes; `[MENU] open`/`closed` in the log. Toolkit hooks it uses (patch 0023):
`xbox_FramebufferWindowSetEventHook` (every SDL event on the window thread first; nonzero keeps it
from the keyboard pad) and `xbox_InputSetFilter` (each pad state on the polling thread; the menu
reads Back+Start there and zeroes the state while open and until the pad is let go). Events are
queued to the present thread, which redraws the last frame every 16 ms while open; closed it
costs one pointer check per present. While open the game is frozen (B7a2): `mksm_gpu_set_frozen`
holds the method thread at the next flip after presenting it (`[XGPU] frozen at flip N` /
`resumed after N ms`), so the title blocks on the GPU and its tick-per-pass loop stops (no time
jump: frames after a 5 s freeze in a movie match a no-freeze run byte for byte), and the toolkit's
`mcpx_apu_set_host_paused` (patch 0024) stops APU frames and the audio device in place (`[APU]
host paused`/`resumed`). Pads are still polled while frozen (the USB thread keeps answering the
title's transfers), so Back+Start and pad nav work; `[MENU] closed (N pad polls while open)`.
Keys
come from SDL events (`keyboard_sdl.c`), pads from SDL's game controllers (B9b1, below). The GDI window only:
`RECOMP_FB_DUMP` (the SDL window has none) needs an absolute path (the run
scripts start the exe in `build/Release`). For a renderer drawing into the window itself it
exports the HWND, a "GPU presents" switch (no GDI blit) and the client size in physical pixels;
the GPU path uses them (plan 2.6b), so `RECOMP_FB_DUMP` sees only an occasional sampled frame
there: use `RECOMP_PB_GPU_DUMP`, or `RECOMP_PB_GPU_PRESENT=pbo`.
`tools/run.bat` is the cmd twin (same defaults and overrides). It writes the log without a live
console view and prints the last 40 lines on exit; in cmd, turn `RECOMP_AC97_READY` off with `0`
(empty means unset there). Keep the two scripts' defaults in sync.
The failure path for a missing XBE shows a MessageBox, which blocks an unattended run.
Saves and settings (plan B7b1): `main.c` passes the toolkit a title-specific base dir,
`%LOCALAPPDATA%\mksm_recomp` (`resolve_base_dir`): the emulated hard disk (`Partition0-5.img`,
sparse, caches) and `settings.ini`. The saves (`UDATA\4d570029`, `TDATA`) go to their own folder,
`<base>\saves`, or `MKSM_SAVE_DIR` (which now names that folder only), handed to the toolkit as
`RECOMP_SAVE_DIR` (its switch that moves just UDATA/TDATA; one set by hand wins). Logged as
`[BOOT] save dir: <saves> (disk images: <base>)`. Moved there once at boot (`[SAVE] moved`):
`<base>\UDATA`/`TDATA` (builds before B7b1; not when `MKSM_SAVE_DIR` comes from the environment,
so a dev folder never takes the player's saves) and `<game>\UDATA`/`TDATA` (builds before
toolkit v0.14); a failed move from the game dir (another volume) leaves `RECOMP_SAVE_DIR` unset so
the toolkit keeps using them there. Older builds used the shared `%LOCALAPPDATA%\xboxrecomp`; it
can be deleted.
`settings.ini` (`src/settings.c`, read first thing in `WinMain`, before anything reads the
variables): each key stands for an env var (`scale` = `RECOMP_PB_GPU_SCALE`, `aspect`, `wide`,
`hud_edges`, `movie_zoom`, `gauntlet_zoom`, `filter`, `volume`, `fullscreen`, `save_dir` =
`MKSM_SAVE_DIR`; table in `README.md`) and is `_putenv_s`'d unless that variable is already set
(`[SETTINGS] key=value` / `... is set, file value ... not used`), so the readers stay as they
are. Missing file = written with the monitor defaults (D3 (2): `EnumDisplaySettings` of the
primary monitor; `[SETTINGS] wrote`). `RECOMP_SETTINGS=0` skips it (the run scripts and the
smoke test default to 0, so dev runs keep the Xbox look), `=1` or unset = `<base>\settings.ini`,
anything else = that path (handy for tests: a missing path is written fresh).
The menu edits it (B7b2, `menu.cpp`): its copy starts from `mksm_settings_file()` (the file as
read at start); live items are saved at once (`mksm_settings_save`: temp file + replace, unknown
`key=value` lines kept; they apply at the next start until B7c), scale/aspect/wide stay pending
until "Apply and restart" (confirm popup) saves and calls `mksm_settings_restart`: it drops the
variables the file set (`mksm_settings_env_from_file`) and the derived ones registered with
`mksm_settings_derived_env` (`RECOMP_SAVE_DIR`) from the process environment, starts the exe
again with the same command line, working dir and std handles (same log) and
`MKSM_RESTART_PARENT=<pid>`, then terminates; the new process waits for that pid to be gone
before it reads the file (`[SETTINGS] restarting (new process N)` / `restarted (old process N
gone)`). A variable the user set shows as "set by ..." and isn't editable.

## Testing
Unit tests for the Python tools (stdlib `unittest`, pytest also runs them; `stub_audit` needs
capstone and is skipped without it). They build tiny synthetic WADs/ISOs/sections in a temp dir,
so they need no game data. Run them after changing a tool:
```bash
python -m unittest discover -s tests  # or: ctest --test-dir build -C Release -R tools
```
`gpu_link` / `gpu_link_gl` (CTest, ~1 s each): a cl/link.exe exe that brings up xemu's PGRAPH through
`mksm_gpu_selftest()` with the null renderer / the OpenGL renderer (`gpu_link gl`, needs a GL 4.0 core
driver; prints the GL version and device); run them after touching `src/gpu`. Linking the GL renderer
means GL contexts are created in both modes (xemu's `nv2a_context_init` runs every renderer's
`early_context_init`).
Run the smoke test after every re-lift or rebuild (a re-lift can break a path that used to work):
```bash
python tools/smoke_test.py            # or: ctest --test-dir build -C Release --output-on-failure
```
It runs the exe headless and muted (`RECOMP_APU_VOLUME=0`; `--audio` to hear it, a volume set in
the environment wins) with a watchdog per checkpoint (~3.5 min total), logs to
`../logs/smoke_<timestamp>_<checkpoint>.log`, and checks: exit 0/3 (3 = watchdog, normal), no
`[CRASH]`, no `Failed to resolve VA`, no `is not code` (except `KNOWN_WILD`), no `arena exhausted`,
a DSP doorbell ack, a minimum draw count,
and files opened. `boot` = no input, both intro movies; `pad` = scripted Start/A past Select
Difficulty and the movie skip into the level load (`WADCRC.BIN`). Draw counts depend on the frame rate, so prefer file markers when adding checkpoints.
Add a checkpoint each time the game gets further. Game dir: `--game-dir`, else `MKSM_GAME_DIR`,
else `../game_files`; without game data it exits 77 (CTest skip). `--only boot|pad` runs one.

Frame capture (for the GPU renderer work): `--capture-ms MS | --capture-flip N` records one pad
frame as a self-contained `MKPB` file plus a software-reference `.bmp` (toolkit patches 0005-0007),
running only the `pad` checkpoint. `--capture-ms` (arm at the first flip after that many ms, counted
from the first pushbuffer method; the run lasts at least MS/1000 + 15 s) is the reproducible choice;
`--capture-flip` is the Nth `FLIP_STALL`. The two are mutually exclusive. `--capture-path` sets the file path
(default `../logs/pbcap_level_<N>.pbcap`). The test then requires `[PBCAP] wrote` after `WADCRC.BIN`
opens and that the `.ref.bmp` is non-black (>= 30000 px). Replay a capture with `RECOMP_PB_REPLAY=<path>`
and byte-compare `<path>.ref.bmp` vs `<path>.replay.bmp`. Captures are ~144 MB; clean up old ones.

The checkpoints run on the GPU path by default (`RECOMP_PB_GPU=1`, xemu's PGRAPH, OpenGL
renderer, like the run scripts; user, 2026-10-16): draws come from the `[XGPU]` stats line,
which must show 0 bad handles, and the renderer must come up; with OpenGL, frames must have
been presented and one of them non-black. This is the gate after a change. `--software` runs
them on the software executor (`RECOMP_PB_GPU=0`); it has rendering glitches of its own, so
it is only an occasional "does it still run" check, not a gate or a reference for visual bugs
(compare against xemu). Captures always run on the software executor. `--gpu` is still
accepted and does nothing. Every run
also fails on `Assertion failed` (xemu keeps its asserts on; `abort()` exits 3 like the watchdog).

## Input
The game's statically linked XAPI drives the USB host controllers directly, so pads come from the
toolkit's emulated OHCI (`RECOMP_USB`, wired up in `main.c`: `xbox_OhciInit` + the VEH MMIO trap).
The run scripts default `RECOMP_USB=1` and `RECOMP_KEYBOARD=1`. Host pad n drives Xbox pad n: with
the SDL window (the default build) through SDL2's game controller API (toolkit patch 0022, plan
B9b1: any controller SDL knows, ports in SDL's device order, an empty port rescanned once a second
so pads can be plugged in while running, read also in the background as XInput was); `main.c`
calls `xbox_InputInit` on the main thread before the USB thread starts. `-DMKSM_SDL_WINDOW=OFF`
keeps XInput. Log: `[PAD] port N: <name> (SDL game controller)` / `disconnected` / `Back+Start
held` (the menu combo for B7a). The mouse+keyboard pad (`RECOMP_MKB`, F9; toolkit `mkb_input.c`) reads the SDL
window's events since B9b2 (patch 0025): the window thread hands each event to `mkb_sdl_event`
after the menu hook and before the key table, and `mkb_sdl_tick` turns SDL's relative mouse mode
on only while the window has keyboard focus, F9 hasn't released it and the menu is closed
(`mkb_set_overlay`, called from `menu.cpp`'s `set_open`). Headless there is no window loop, so it
never captures. `RECOMP_MKB_TRACE=1` logs each `relative mode on/off`. The GDI build keeps the
Win32 raw-input thread.
The keyboard is merged on top and read from the framebuffer window (`RECOMP_FB_WINDOW=1`, needs focus):
arrows = d-pad, Enter = Start, Backspace = Back, Z/X/A/S = A/B/X/Y, Q/E = white/black,
1/3 = triggers, numpad 8/2/4/6 = left stick, I/K/J/L = right stick, Shift/Ctrl = stick clicks.
Headless: `RECOMP_PAD_PRESS=0x10,3000,300` (mask,period,hold ms) or `RECOMP_PAD_SCRIPT`.
`RECOMP_USB_STATS=1` prints transfer counts every 5 s.

## Debugging
Expect crashes during bring-up. The VEH handler in `main.c` prints the faulting guest function, the
guest stack, and recent ICALL targets. Fix by adding overrides in `recomp_manual.c` or runtime
support. Useful env vars from the toolkit: `RECOMP_WATCHDOG_SECS`, `RECOMP_AC97_READY`.
`RECOMP_PB_GPU=1` (default in the run scripts since 2.H.8c1, `=0` = software executor; toolkit patches
0008-0009) hands every decoded method to xemu's PGRAPH
(OpenGL renderer) instead of the software executor (`[XGPU]` lines every >= 5 s, checked every 16k methods,
so during movies later; each line ends with its real interval in seconds, which `tools/profsum.py` divides by); each flip presents the
surface drawn last into the framebuffer window (`RECOMP_FB_WINDOW=1`) and waits for a 59.94 Hz VBlank.
`RECOMP_PB_GPU_RENDERER=null` = xemu's null renderer (draws nothing), `RECOMP_PB_GPU_VBLANK=<Hz>` sets
the pacing (0 = uncapped), `RECOMP_PB_GPU_DUMP=<file.bmp>` (+ `RECOMP_PB_GPU_DUMP_FLIP=<N>[,<N>...]`,
default 600, up to 16 flips; with more than one each file is `<file>_<flip>.bmp`) dumps presented
frames. `RECOMP_PB_GPU_SCALE=1..4` (plan 4.1, default 1) sets xemu's internal
resolution scale at start-up: surfaces render at Nx, CPU reads of surfaces are downscaled to 1x,
the window shows the scaled surface (the dump and the `pbo` path see it at Nx too); not changeable
while running. Above 1x, `src/gpu/shader_patch.c` edits xemu's generated GLSL as it reaches
`glShaderSource` (plan B2b): screen-space vertices (w = 1 or rhw = 0) snap to the 1x pixel grid
(`ceil(x - 0.5)`) and point-sampled stages read their texels at the 1x pixel centre, so the 2D
layer (HUD, bars, menus) shows what 1x shows; it also hooks `glUseProgram`/`glLinkProgram`/the
draw calls to set a per-draw uniform with the point-sampled stages. `RECOMP_PB_GPU_SNAP2D=0`
turns it off, `=debug` colours 2D draws (red/green = sub-pixel x/y of a vertex, blue = 0.25,
+0.5 for w = 1, +0.25 if stage 0 is point-sampled). `RECOMP_ASPECT=16:9` (plan 4.2, B5a; also `16:10`, `21:9`, `W/H` or a ratio; unset/4:3 = off) draws
the 3D view at that aspect (Hor+: entry hook on the camera update in `recomp_manual.c`, see
`--exclude-manual`) and the window present stretches the 640x480 frame to it (anamorphic, so the
HUD/menus stretch until B5b), except during movies (kept 4:3). The dump shows the unstretched
frame. `RECOMP_ASPECT_WIDE=1` (B5b1, with `RECOMP_ASPECT`) renders into a wider back buffer
instead (W x 480, 854 at 16:9, at most 1024: wider buffers get stripes, so wider aspects are
stretched the rest of the way at present, plan B5b3; hooks on the display init `0x0011B290` (a `_gen` wrapper), the
device reset and D3D's display-mode lookup, which only knows 640/720-wide modes and is handed
640): square pixels, the present and the dump show the W-wide frame. The 2D layer (authored
for 640) is pillarboxed (B5b2): the game keeps seeing a 640-wide display, so its layout is the
4:3 one; wrappers give the real W only to the 2D projections, viewports and full-screen
quads, pixel-space rects move by (W-640)/2, full-width menu sprites and the bars widen to W, movies keep a 640x480 buffer (pillarboxed by the
present), and in the menus (the game's front-end flag) the GPU glue blacks out the columns
outside the centred 640 at each flip (`mksm_gpu_set_side_mask`): their backdrops have art there.
Movie zoom (plan B5c2, on by default with `RECOMP_ASPECT`; without it movies stay as on the Xbox): letterboxed movies are zoomed
by the window present as far as the window is wider than 4:3, up to the share the picture fills
(a per-file table in `recomp_manual.c`, keyed by the name the movie player gets; `[MOVIE]` logs
it), cropping only black rows; the others stay pillarboxed. `RECOMP_MOVIE_ZOOM=0` turns it off,
`RECOMP_MOVIE_ZOOM_GAUNTLET=1` also zooms the Gauntlet trailer (cuts its full-frame shots). The
dump shows the unzoomed frame. `RECOMP_CINE_LOG=1` prints `[cine]` lines when the cutscene signals change (sequence
flag, cinematic camera, letterbox bars, fade) and at movie start/end.
With the wide buffer the HUD sits at the screen edges (plan B5d2, `RECOMP_HUD_EDGES=0` keeps
it at its 4:3 place in the centre 640): `recomp_manual.c` records the HUD manager's elements
as the game builds them, moves each left/right piece by ∓(W-640)/2 for its draw (centred ones
stay) and moves the text an element's callback queues the same way.
`RECOMP_HUD_LOG=1` (plan B5d1) logs every 300th sprite flush: the drawn sprites (position, size,
anchor flags, HUD side) and the next frame's queued text and rect quads with the HUD element
whose callback queued them (`[HUD]` lines).
`RECOMP_PB_GPU_DIRTY=always` makes every texture/vertex range read as
changed on every use (default: once per pushbuffer kick, texture pages only when their content hash
changed, see `docs/gpu-renderer.md`; for A/B runs); `=kick` drops the texture-page hash.
By default (`RECOMP_PB_GPU_PRESENT=window`, plan 2.6b) the method thread draws the scanout surface
straight into the game window: a GPU copy handed to a present thread that owns a second GL context
on the window's DC (aspect-fit, black bars, `SwapBuffers`, vsync off; `RECOMP_PB_GPU_FILTER=nearest` instead of bilinear); two flips in 256 are
also read back for the `[XGPU]` non-black count and `RECOMP_PB_GPU_DUMP`. Until the window exists
(and always headless) frames go the `pbo` way: `RECOMP_PB_GPU_PRESENT=pbo` reads the scanout surface
into a pixel-pack buffer and hands it to the GDI window one flip later (2.H.8c2b);
`RECOMP_PB_GPU_PRESENT=sync` presents through xemu's display pass and a synchronous readback (the
pre-2.H.8c2b path, for A/B). The `[XGPU]` line also counts xemu's surface downloads/uploads/surface-to-texture.
`RECOMP_PB_GPU_GLCHECK=1` makes `glGetError` real again (xemu asserts it per draw; by default the shim
answers `GL_NO_ERROR`, a guess that each call can stall NVIDIA's threaded driver; not measured alone).
Movies (Sofdec) play on this path too (user, 2026-10-15, after 2.H.8c1). It still needs
`RECOMP_PB_EXEC=1`, which is what makes the ack thread decode the pushbuffer.
`RECOMP_PROF=<start_ms>,<dur_ms>[,<period_us>]`: sampling profiler (`src/prof.c`). After start_ms it
samples every thread's stack each period (default 1000 us) for dur_ms, then prints `[PROF]` per thread
with >= 2% CPU: CPU time, the thread's name (else its start routine), top exclusive/inclusive functions (`module!symbol`; guest
code shows as `sub_XXXXXXXX`, the renderer lib has /Z7 debug info). For the level with the `pad`
script: `RECOMP_PROF=62000,15000 python tools/smoke_test.py --only pad`. Threads are named at creation
(`SetThreadDescription` via the toolkit's header-only `xbox_name_thread`, `<xbox/thread_name.h>`, patch
0015): guest threads `guest #<n> <routine>(<ctx1>)` (ctx1 = the title's routine behind XAPI's common
startup), runtime threads by role (`nv2a ack/pushbuffer` = the method thread, `wbc ack`, `gpu present`,
`kernel timers`, `ohci usb`, `fb window`, `watchdog`, ...); unnamed ones (driver threads) keep the
symbol. Name any new thread the same way. Debuggers and WPA show the names too.
Summarize: `python tools/profsum.py [--inclusive] LOG...` (median per thread over several logs). Measure only on an
idle machine (no game running: a background game cut the level from 60 to 12-32 fps, plan 2.7a2).
`RECOMP_PROF_FOCUS=<text>` also prints each thread's most common call chains through the first frame whose
symbol contains the text (who calls it). The report takes a few seconds; end the window well before the
watchdog (`62000,15000` for the pad level). The 0xC0000374 exits seen while it printed were a prof.c
bug (a start routine resolved after the per-symbol arrays were sized), fixed in 2.H.8c2a.
Headroom runs with `RECOMP_PB_GPU_VBLANK=0` don't work with the pad script: uncapped menus and movie
run too fast for its time-based steps and it misses the movie skip.
`RECOMP_WBC_ACK` (plan 2.H.8b): a thread acks D3D's kickoff flush wait at once instead of the ack
thread doing it after each executed segment, so the title runs ahead of the executor. **On by default
on the GPU path** (`=0` turns it off for A/B runs, `=1` forces it); it relies on the fence mirror,
software method 5 (patch 0010) and a DMA_GET that moves during the ring walk: upstream's DMA-engine
walk, `RECOMP_PB_WALK=dma` (a run script / smoke default since toolkit v0.14, plan H.5a; the toolkit's
default segment scan loses the tail of each ring lap; with `RECOMP_PB_WALK=` (the segment scan)
and the ack on, the GPU path hangs at boot with 0 flips, plan H.5b1). On the software
executor it hangs (no fence mirror / software methods there), so it stays off there.
`RECOMP_PB_EXEC=1` (set by the run scripts) is required: D3D init waits on GPU semaphore releases,
which only the pushbuffer executor performs.
The run scripts also set `RECOMP_AC97_READY=1` (emulated APU, so `DirectSoundCreate` succeeds) and
`RECOMP_APU_DSP_ACK=auto` (upstream's default since v0.14: acks the GP DSP command doorbell that
DirectSound init waits on, found as GP scratch page 0 + 0x810 via `GPSADDR`, logged as `DSP doorbell
(auto)`; the smoke test checks it). If audio init hangs, compare `ebx` in a watchdog dump with the
logged address; a fixed address can be given instead.
Since toolkit v0.14 (plan H.5a) the APU is upstream's: its output pacing, pitch resampling (linear),
front-end traps and heap-page addressing replaced our patches, and so did their diagnostics. Ours
that are gone: `RECOMP_APU_STATS`, `[APU-VOICE]`/`[APU-DROP]`/`[APU-TOP]`, `RECOMP_APU_RESAMPLE` and
the sinc resampler (deferred: rewrite it onto upstream's resampler if 3.3d's footstep buzz is back),
`RECOMP_APU_DATA_HEAP`, `RECOMP_APU_TRACE_SGE`, `RECOMP_APU_MONITOR`, `RECOMP_APU_TRACE_SECTL`. Old
logs and `docs/audio.md` still mention them. Upstream's own: `RECOMP_AUDIO_WAV=<path>` (what the
audio device is given, `RECOMP_AUDIO_WAV_SECS` limits it; scan with `tools/wavscan.py`),
`RECOMP_APU_RING` (stale-lap replays in streamed rings), `RECOMP_APU_SOLO`, `RECOMP_APU_VOICE_DUMP`
(check its format against `tools/voicedump.py` before relying on that tool), host starvation lines.
`RECOMP_AUDIO_TICK_STATS=1` (`recomp_manual.c`) prints `[AUDIO-TICK]` calls per second of the
game's 5 ms sound driver tick (`timeSetEvent` callback; should be ~200, needs patch 0013's
precise kernel timers: the old 10 ms poll gave ~65).
`RECOMP_APU_VOLUME=<0..1>` sets the output volume (patch 0012; 0 = silent, the APU still runs).
The run scripts and smoke set `RECOMP_AUDIO_BUF_COUNT=8` (upstream's 8 x 512-sample XAudio2 buffers,
85 ms; its default 4 = 43 ms underran several times a second, choppy movies and level, user 2026-10-16;
`RECOMP_AUDIO_BUF_SAMPLES` sets the size). `RECOMP_AUDIO_HOST=1` prints `[AUDIO-HOST]` once a second
(starves, underruns, the longest gap between submits, the lowest queue seen).
`RECOMP_HEAP_RECLAIM=1` (also a default) reuses freed contiguous/heap blocks: without it
the contiguous arena runs out (`[CONTIG] arena exhausted`) and the movie after New Game stays black.
They also set `RECOMP_TITLE_KEVENTS=1` (events the game builds by hand get a host event, so waits
on them block instead of failing at once; the XDK timer thread behind the audio tick needs it) with
`RECOMP_TITLE_KEVENTS_SKIP` (patch 0003; empty by default): events listed there keep the fail-at-once
path. D3D's VBlank event `0x003F29CC` was on it until plan 2.7a6: `main.c`'s `vblank` thread now ticks
at 59.94 Hz (`RECOMP_PB_GPU_VBLANK` if > 0) and has the toolkit set it (`xbox_Nv2aVBlank`, patch 0016:
the event D3D's VBlank DPC sets, miniport + 0x194), so `BlockUntilVerticalBlank` blocks as on hardware
and the title's VBlank-paced workers stop spinning (~2 cores). The GPU path's flips pace on the same
ticks (`mksm_gpu_vblank_tick`). `RECOMP_TITLE_KEVENTS_SKIP=0x003F29CC` = the old spin, for A/B runs.
The same thread stands in for the title's VBlank callback (`title_vblank_callback`, plan B1b):
D3D's DPC calls the one registered at miniport + 0x190, the toolkit doesn't, so the game's VBlank
counters never moved and the loading screen drew no progress bar. The game's callback only counts,
so `main.c` does the same counting itself while that callback is the registered one (an unknown
one is logged, not run). `RECOMP_TITLE_VBLANK_CB=0` turns it off (A/B); `RECOMP_VBLANK_STATS=1`
prints `[VBLANK]` with the counters once a second.
Patch 0017 keeps one pending wake-up per thread for self-suspend / resume pairs (a resume that
arrives while the target is still running is not lost; no switch: the plain path crashed at boot in
`NtResumeThread` once the VBlank wait blocked, not traced), and patch 0018's `RECOMP_APU_EP_ALIVE=0xFE85A018` (run
script / smoke default) keeps DirectSound's EP-alive word at `0xCCCCCC`: without it DirectSound reset
the DSPs and busy-waited 10 ms ~20 times a second. A new hang on a hand-built event → log its VA
(`event 0x... built by the title`) and try adding it to the skip list.
