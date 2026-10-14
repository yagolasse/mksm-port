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
- `src/recomp_manual.c`: hand-written overrides (`recomp_lookup_manual`) and ICALL diagnostics
- `src/recomp/gen/`: generated C (not in git)
- `external/xboxrecomp/`: toolkit git submodule (pinned). Prefer fixing things in our `src/` first;
  toolkit changes should go upstream or into a fork
- `tools/run.sh`, `tools/run.bat`: run the exe with the needed env defaults, log to `../logs/`
- `tools/xiso.py`: list/extract files from an Xbox disc image
- `tools/padding_seeds.py`: finds undetected functions after int3 padding (writes `seeds/padding.json`)
- `tools/stub_audit.py`: sorts `gen/recomp_stubs_unresolved.c` into real code vs. decoded data (writes `seeds/stubs.json`)
- `seeds/`: extra function entry points for `tools.disasm --seed-functions` (see Pipeline)
- `patches/`: our toolkit changes not yet upstream; apply them in `external/xboxrecomp` after cloning
  (`git am ../../patches/*.patch`)

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

## Build
VS 2022 + CMake, x64 (the default; don't force `-A Win32`).
```bash
cmake -S . -B build
cmake --build build --config Release --parallel 6
```
Use `--parallel N`, not `-- /m`: Git Bash rewrites `/m` into a path (`M:/`) and MSBuild fails.
## Run
The exe takes the extracted game folder (holding `default.xbe`) from its first argument, else
`MKSM_GAME_DIR`, else `game\` in the working directory (`resolve_game_dir` in `src/main.c`).
It's a GUI-subsystem exe, so stdout only appears when redirected. Use a run script, which
defaults to `../game_files` and logs to `../logs/run_<timestamp>.log`:
```bash
tools/run.sh                                  # MKSM_GAME_DIR / MKSM_EXE / MKSM_LOG override
RECOMP_WATCHDOG_SECS=10 timeout 60 tools/run.sh
```
`tools/run.bat` is the cmd twin (same defaults and overrides). It writes the log without a live
console view and prints the last 40 lines on exit; in cmd, turn `RECOMP_AC97_READY` off with `0`
(empty means unset there). Keep the two scripts' defaults in sync.
The failure path for a missing XBE shows a MessageBox, which blocks an unattended run.

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
`RECOMP_PB_EXEC=1` (set by the run scripts) is required: D3D init waits on GPU semaphore releases,
which only the pushbuffer executor performs.
The run scripts also set `RECOMP_AC97_READY=1` (emulated APU, so `DirectSoundCreate` succeeds) and
`RECOMP_APU_DSP_ACK=<addr>` (acks the GP DSP command doorbell that DirectSound init waits on). The
doorbell address comes from an allocation; if audio init hangs, read `ebx` from a watchdog dump.
