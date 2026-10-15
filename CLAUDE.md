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

## Layout
- `CMakeLists.txt`: game executable `mksm_recomp`; pulls in the toolkit via `add_subdirectory`
- `src/main.c`: host entry: loads the XBE, maps Xbox memory, inits kernel/bridge, calls the guest entry `0x000F40CA`
- `src/prof.c`: `RECOMP_PROF` sampling profiler (all threads, symbolized from the PDB; see Debugging)
- `src/recomp_manual.c`: hand-written overrides (`recomp_lookup_manual`) and ICALL diagnostics
- `src/recomp/gen/`: generated C (not in git)
- `src/gpu/`: GPU renderer library `mksm_gpu` (xemu's PGRAPH; the game uses it with `RECOMP_PB_GPU=1`): its own
  CMake project built with clang-cl (see Build). `xemu/hw/xbox/nv2a/` = xemu files **copied unmodified**
  (same paths as in xemu, so they diff cleanly; never edit them, fix things in `xemu/shim/` or the glue);
  `xemu/include/`, `xemu/util/` = QEMU headers/sources from xemu's tree that the GL renderer uses
  (`qemu/lru.h`, `queue.h`, `mstring.h`, `fast-hash`), also unmodified;
  `xemu/shim/` = stand-ins for the QEMU/glib/xemu headers they include (incl. `epoxy/gl.h` → glad);
  `nv2a_vsh_cpu/`, `xxhash/` = vendored libraries; `glad/` = generated GL 4.6 core/WGL loader;
  `gloffscreen_wgl.c` = xemu's GL context layer on WGL (hidden windows, newest 4.x core, shared contexts);
  `xemu_stubs.c` = externals from unported xemu files; `mksm_gpu.c` = stands in for xemu's PFIFO
  (RAMHT lookup, acks the NOP-notify and flip-stall waits, presents the last-drawn surface at the
  flip and paces flips to 59.94 Hz); `mksm_gpu.h` = the plain-C interface for
  cl-built code. Licenses: `LICENSE` (GPL-3.0, the combined work), `LICENSES/`, `NOTICE` (file → license)
- `external/xboxrecomp/`: toolkit git submodule (pinned). Prefer fixing things in our `src/` first;
  toolkit changes should go upstream or into a fork
- `tools/run.sh`, `tools/run.bat`: run the exe with the needed env defaults, log to `../logs/`
- `tools/smoke_test.py`: boot smoke test (also CTest `smoke`), see Testing
- `tools/xiso.py`: list/extract files from an Xbox disc image
- `tools/padding_seeds.py`: finds undetected functions after int3 padding (writes `seeds/padding.json`)
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
    --seed-functions ../../seeds/stubs.json  # all code sections
python -m tools.func_id ../../../game_files/default.xbe -v
python -m tools.abi_analysis ../../../game_files/default.xbe -v
bash ../../tools/ghidra/run_ghidra.sh   # our wrapper (Java export; Ghidra 12 has no Jython), ~20+ min
python tools/ghidra_naming/merge_names.py --apply
python -m tools.recomp ../../../game_files/default.xbe --all --split 250 --gen-dir ../../src/recomp/gen
```
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
  padding). A call or jump to an undetected address lifts to a silent no-op stub in
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
`tools/run.bat` is the cmd twin (same defaults and overrides). It writes the log without a live
console view and prints the last 40 lines on exit; in cmd, turn `RECOMP_AC97_READY` off with `0`
(empty means unset there). Keep the two scripts' defaults in sync.
The failure path for a missing XBE shows a MessageBox, which blocks an unattended run.

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
It runs the exe headless with a watchdog per checkpoint (~3.5 min total), logs to
`../logs/smoke_<timestamp>_<checkpoint>.log`, and checks: exit 0/3 (3 = watchdog, normal), no
`[CRASH]`, no `Failed to resolve VA`, no `is not code` (except `KNOWN_WILD`), no `arena exhausted`,
a DSP doorbell ack, a minimum draw count,
and files opened. `boot` = no input, both intro movies; `pad` = scripted Start/A past Select
Difficulty and the movie skip into the level load (`WADCRC.BIN`). Draw counts depend on the frame rate, so prefer file markers when adding checkpoints.
Add a checkpoint each time the game gets further. Game dir: `--game-dir`, else `MKSM_GAME_DIR`,
else `../game_files`; without game data it exits 77 (CTest skip). `--only boot|pad` runs one.

Frame capture (for the GPU renderer work): `--capture-ms MS | --capture-flip N` records one pad
frame as a self-contained `MKPB` file plus a software-reference `.bmp` (toolkit patches 0006-0008),
running only the `pad` checkpoint. `--capture-ms` (arm at the first flip after that many ms, counted
from the first pushbuffer method; the run lasts at least MS/1000 + 15 s) is the reproducible choice;
`--capture-flip` is the Nth `FLIP_STALL`. The two are mutually exclusive. `--capture-path` sets the file path
(default `../logs/pbcap_level_<N>.pbcap`). The test then requires `[PBCAP] wrote` after `WADCRC.BIN`
opens and that the `.ref.bmp` is non-black (>= 30000 px). Replay a capture with `RECOMP_PB_REPLAY=<path>`
and byte-compare `<path>.ref.bmp` vs `<path>.replay.bmp`. Captures are ~144 MB; clean up old ones.

`--gpu` runs the checkpoints with `RECOMP_PB_GPU=1` (xemu's PGRAPH, OpenGL renderer): draws come
from the `[XGPU]` stats line, which must show 0 bad handles, and the renderer must come up; with
OpenGL, frames must have been presented and one of them non-black. Every run
also fails on `Assertion failed` (xemu keeps its asserts on; `abort()` exits 3 like the watchdog).

## Input
The game's statically linked XAPI drives the USB host controllers directly, so pads come from the
toolkit's emulated OHCI (`RECOMP_USB`, wired up in `main.c`: `xbox_OhciInit` + the VEH MMIO trap).
The run scripts default `RECOMP_USB=1` and `RECOMP_KEYBOARD=1`. Host XInput pad 1 drives Xbox pad 1.
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
0009-0010) hands every decoded method to xemu's PGRAPH
(OpenGL renderer) instead of the software executor (`[XGPU]` lines every 5 s); each flip presents the
surface drawn last into the framebuffer window (`RECOMP_FB_WINDOW=1`) and waits for a 59.94 Hz VBlank.
`RECOMP_PB_GPU_RENDERER=null` = xemu's null renderer (draws nothing), `RECOMP_PB_GPU_VBLANK=<Hz>` sets
the pacing (0 = uncapped), `RECOMP_PB_GPU_DUMP=<file.bmp>` (+ `RECOMP_PB_GPU_DUMP_FLIP=<N>`, default
600) dumps one presented frame. `RECOMP_PB_GPU_DIRTY=always` makes every texture/vertex range read as
changed on every use (default: once per pushbuffer kick, see `docs/gpu-renderer.md`; for A/B runs).
`RECOMP_PB_GPU_GLCHECK=1` makes `glGetError` real again (xemu asserts it per draw; by default the shim
answers `GL_NO_ERROR`, a guess that each call can stall NVIDIA's threaded driver; not measured alone).
Movies (Sofdec) play on this path too (user, 2026-10-15, after 2.H.8c1). It still needs
`RECOMP_PB_EXEC=1`, which is what makes the ack thread decode the pushbuffer.
`RECOMP_PROF=<start_ms>,<dur_ms>[,<period_us>]`: sampling profiler (`src/prof.c`). After start_ms it
samples every thread's stack each period (default 1000 us) for dur_ms, then prints `[PROF]` per thread
with >= 2% CPU: CPU time, start routine, top exclusive/inclusive functions (`module!symbol`; guest
code shows as `sub_XXXXXXXX`, the renderer lib has /Z7 debug info). For the level with the `pad`
script: `RECOMP_PROF=65000,20000 python tools/smoke_test.py --gpu --only pad`.
`RECOMP_PROF_FOCUS=<text>` also prints each thread's most common call chains through the first frame whose
symbol contains the text (who calls it). Keep the window ending a few seconds before the watchdog: a
65000,20000 run once died with 0xC0000374 while the report was still printing at the 90 s watchdog.
`RECOMP_WBC_ACK` (plan 2.H.8b): a thread acks D3D's kickoff flush wait at once instead of the ack
thread doing it after each executed segment, so the title runs ahead of the executor. **On by default
on the GPU path** (`=0` turns it off for A/B runs, `=1` forces it); it relies on the fence mirror,
software method 5 and a DMA_GET that moves during the ring walk (patches 0012-0014). On the software
executor it hangs (no fence mirror / software methods there), so it stays off there.
`RECOMP_PB_EXEC=1` (set by the run scripts) is required: D3D init waits on GPU semaphore releases,
which only the pushbuffer executor performs.
The run scripts also set `RECOMP_AC97_READY=1` (emulated APU, so `DirectSoundCreate` succeeds) and
`RECOMP_APU_DSP_ACK=auto` (patch 0004: acks the GP DSP command doorbell that DirectSound init waits
on, found as GP scratch page 0 + 0x810 via `GPSADDR` and logged as `DSP doorbell auto`). If audio
init hangs, compare `ebx` in a watchdog dump with the logged address; a fixed address can be given
instead. `RECOMP_HEAP_RECLAIM=1` (also a default) reuses freed contiguous/heap blocks: without it
the contiguous arena runs out (`[CONTIG] arena exhausted`) and the movie after New Game stays black.
They also set `RECOMP_TITLE_KEVENTS=1` (events the game builds by hand get a host event, so waits
on them block instead of failing at once; the XDK timer thread behind the audio tick needs it) with
`RECOMP_TITLE_KEVENTS_SKIP=0x003F29CC` (patch 0003): D3D's event, set by a GPU interrupt the runtime
doesn't model, keeps the fail-at-once path, else the boot hangs. A new hang on a hand-built event
→ log its VA (`event 0x... built by the title`) and try adding it to the skip list.
