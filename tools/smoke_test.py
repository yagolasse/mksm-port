#!/usr/bin/env python3
"""Boot smoke test: run the recompiled exe headless and check its log.

Each checkpoint runs the exe once with a watchdog (RECOMP_WATCHDOG_SECS is the
total run time; exit code 3 means the watchdog ended the run, which is normal
here), then checks the log for markers of how far the game got:

  boot  no input, ~90 s: no crash, no unresolved/wild indirect calls, enough
        draws, both intro movies (MIDWAY.SFD, OPENING.SFD) opened
  pad   scripted pad (Start x5, then A, then Start): the save meta is read,
        the character pick opens GAMEDATA.WAH, the draw count reaches Select
        Difficulty, accepting it (step 9) opens OPENING.SFD (the story intro),
        and skipping that movie starts the level load (WADCRC.BIN)

Run it after every re-lift or rebuild: a re-lift can break a path that used to
work. When the game gets further, add a checkpoint here.

Usage:
    python tools/smoke_test.py [--game-dir DIR] [--exe EXE] [--log-dir DIR]
                               [--only boot|pad]

The game dir (holding default.xbe) defaults to MKSM_GAME_DIR, else
../game_files. No game data lives in this repo; without it the test exits 77
(CTest's skip code). Exit 0 = all checks passed, 1 = a check failed.

The env defaults match tools/run.sh and tools/run.bat: keep the three in sync.
"""

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
SKIP = 77

# Same defaults as tools/run.sh / tools/run.bat.
RUN_ENV = {
    "RECOMP_PB_EXEC": "1",
    "RECOMP_AC97_READY": "1",
    "RECOMP_USB": "1",
    "RECOMP_KEYBOARD": "1",
    "RECOMP_APU_DSP_ACK": "0x83550810",
    "RECOMP_TITLE_KEVENTS": "1",
    "RECOMP_TITLE_KEVENTS_SKIP": "0x003F29CC",
}

# The 1.12c pad script: Start through the title, brightness and main menu,
# then A through character select into Select Difficulty and past it.
# Times are ms from the pad's enumeration.
PAD_SCRIPT = ",".join(
    [f"{t}:start" for t in (4000, 8000, 12000, 16000, 20000)]
    + [f"{t}:a" for t in (26000, 30000, 34000, 38000, 42000, 48000, 54000)]
)

# 1.15d: A at 54 s or Start at 57 s skips OPENING.SFD and the level load
# starts. From then on the game renders at ~5 fps and polls the pad only every
# ~10 s, so later steps are missed; nothing past this one is scripted.
PAD_SKIP_MOVIE = "57000:start"

# Wild indirect calls that are known and harmless: reported, not failed.
# 0x00000140: seen once in a 90 s run after 1.12c, not reproducible (PLAN 1.12c).
KNOWN_WILD = {0x00000140}


class Checkpoint:
    def __init__(self, name, secs, env, min_draws, files, last_pad_step=None,
                 files_after_step=None):
        self.name = name
        self.secs = secs
        self.env = env
        self.min_draws = min_draws
        self.files = files            # path suffixes that must show up as [PATH]
        self.last_pad_step = last_pad_step
        # (step, [suffixes]): files that must open after that pad step's line
        self.files_after_step = files_after_step


CHECKPOINTS = [
    # 1.12c: 38,024 draws in 90 s, OPENING.SFD opens at ~80 s.
    Checkpoint("boot", 95, {}, 30000, ["MIDWAY.SFD", "OPENING.SFD"]),
    # Draws depend on the frame rate: 40,436 after accepting in 1.12c, 34,391 in
    # 1.13 (same path). GAMEDATA.WAH opens after picking a character (step 7),
    # just before Select Difficulty, so it is the sturdier marker.
    # 1.15b: accepting Select Difficulty (step 9) opens OPENING.SFD. Before the
    # timer thread worked (boot-log #5) nothing opened after the accept.
    # 1.15d: skipping the movie opens WADCRC.BIN (only then) at 55-60 s, then
    # level data; 90 s leaves time for the slow frames after it.
    Checkpoint("pad", 90, {"RECOMP_PAD_SCRIPT": PAD_SCRIPT + "," + PAD_SKIP_MOVIE}, 25000,
               ["MIDWAY.SFD", "SaveMeta.xbx", "GAMEDATA.WAH"], last_pad_step=9,
               files_after_step=(9, ["OPENING.SFD", "WADCRC.BIN"])),
]

RE_DRAWS = re.compile(r"^\[GPU\] draws (\d+) ", re.M)
RE_PATH = re.compile(r"^\s*\[PATH\] (.+)$", re.M)
RE_WILD = re.compile(r"\[ICALL\] target 0x([0-9A-Fa-f]+) is not code")
RE_PAD = re.compile(r"PAD: t=\d+ ms step (\d+)")
RE_PAD_STEP = r"PAD: t=\d+ ms step %d "


def run(cp, exe, game_dir, log):
    env = dict(os.environ)
    env.update(RUN_ENV)
    env.update(cp.env)
    env["RECOMP_WATCHDOG_SECS"] = str(cp.secs)
    print(f"[{cp.name}] running {cp.secs} s, log {log}", flush=True)
    start = time.monotonic()
    with open(log, "wb") as f:
        try:
            # From the exe's folder, like the run scripts.
            code = subprocess.run([str(exe), str(game_dir)], cwd=exe.parent, env=env,
                                  stdout=f, stderr=subprocess.STDOUT,
                                  timeout=cp.secs + 60).returncode
        except subprocess.TimeoutExpired:
            code = None
    print(f"[{cp.name}] exit {code} after {time.monotonic() - start:.0f} s", flush=True)
    return code


def check(cp, code, text):
    """Return a list of (ok, message)."""
    results = []
    results.append((code in (0, 3), f"exit code {code} (0 or 3 = watchdog expected)"))
    crashes = text.count("[CRASH]")
    results.append((crashes == 0, f"{crashes} [CRASH]"))
    unresolved = sorted(set(re.findall(r"Failed to resolve VA (0x[0-9A-Fa-f]+)", text)))
    results.append((not unresolved, f"{len(unresolved)} unresolved ICALL targets "
                    + " ".join(unresolved[:10])))
    wild = {int(v, 16) for v in RE_WILD.findall(text)}
    bad = sorted(wild - KNOWN_WILD)
    results.append((not bad, f"{len(bad)} wild ICALL targets "
                    + " ".join(f"0x{v:08X}" for v in bad)))
    for v in sorted(wild & KNOWN_WILD):
        print(f"  note: known wild ICALL 0x{v:08X}")
    draws = [int(d) for d in RE_DRAWS.findall(text)]
    n = max(draws, default=0)
    results.append((n >= cp.min_draws, f"{n} draws (need >= {cp.min_draws})"))
    paths = RE_PATH.findall(text)
    for name in cp.files:
        hit = any(p.lower().endswith(name.lower()) for p in paths)
        results.append((hit, f"opened {name}"))
    if cp.last_pad_step is not None:
        steps = [int(s) for s in RE_PAD.findall(text)]
        last = max(steps, default=-1)
        results.append((last >= cp.last_pad_step,
                        f"pad script reached step {last} (need >= {cp.last_pad_step})"))
    if cp.files_after_step is not None:
        step, names = cp.files_after_step
        m = re.search(RE_PAD_STEP % step, text)
        later = RE_PATH.findall(text, m.end()) if m else []
        for name in names:
            hit = any(p.lower().endswith(name.lower()) for p in later)
            results.append((hit, f"opened {name} after pad step {step}"))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game-dir", default=os.environ.get("MKSM_GAME_DIR",
                                                         str(PORT.parent / "game_files")))
    ap.add_argument("--exe", default=str(PORT / "build" / "Release" / "mksm_recomp.exe"))
    ap.add_argument("--log-dir", default=str(PORT.parent / "logs"))
    ap.add_argument("--only", choices=[c.name for c in CHECKPOINTS], action="append")
    args = ap.parse_args()

    game_dir = Path(args.game_dir).resolve()
    exe = Path(args.exe).resolve()
    if not (game_dir / "default.xbe").is_file():
        print(f"SKIP: no default.xbe in {game_dir} (set MKSM_GAME_DIR or --game-dir)")
        return SKIP
    if not exe.is_file():
        print(f"FAIL: exe not found: {exe}")
        return 1
    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")

    failed = []
    for cp in CHECKPOINTS:
        if args.only and cp.name not in args.only:
            continue
        log = (log_dir / f"smoke_{stamp}_{cp.name}.log").resolve()
        code = run(cp, exe, game_dir, log)
        text = log.read_text(encoding="utf-8", errors="replace")
        for ok, msg in check(cp, code, text):
            print(f"  {'ok  ' if ok else 'FAIL'} {msg}")
            if not ok:
                failed.append(f"{cp.name}: {msg}")
    if failed:
        print(f"FAILED ({len(failed)}):")
        for f in failed:
            print(f"  {f}")
        return 1
    print("PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
