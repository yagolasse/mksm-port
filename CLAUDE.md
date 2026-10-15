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
- `tools/smoke_test.py`: boot smoke test (also CTest `smoke`), see Testing
- `tools/xiso.py`: list/extract files from an Xbox disc image
- `tools/padding_seeds.py`: finds undetected functions after int3 padding (writes `seeds/padding.json`)
- `tools/stub_audit.py`: sorts `gen/recomp_stubs_unresolved.c` into real code vs. decoded data (writes `seeds/stubs.json`)
- `docs/`: generic notes on how the port works (`gpu-renderer.md`: design of the pushbuffer → D3D11 renderer)
- `tests/`: unit tests for `tools/*.py` on synthetic inputs (no game data), see Testing
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

## Testing
Unit tests for the Python tools (stdlib `unittest`, pytest also runs them; `stub_audit` needs
capstone and is skipped without it). They build tiny synthetic WADs/ISOs/sections in a temp dir,
so they need no game data. Run them after changing a tool:
```bash
python -m unittest discover -s tests  # or: ctest --test-dir build -C Release -R tools
```
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
