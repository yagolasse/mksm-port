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
                               [--capture-ms MS | --capture-flip N] [--capture-path PATH]
                               [--gpu]

The game dir (holding default.xbe) defaults to MKSM_GAME_DIR, else
../game_files. No game data lives in this repo; without it the test exits 77
(CTest's skip code). Exit 0 = all checks passed, 1 = a check failed.

The env defaults match tools/run.sh and tools/run.bat: keep the three in sync.

--capture-ms / --capture-flip record one pad frame into a pushbuffer capture
(the MKPB file + a software-reference .bmp) by setting RECOMP_PB_CAPTURE plus
RECOMP_PB_CAPTURE_MS (arm at the first flip after that many ms; reproducible,
since the pad script is wall-clock based) or RECOMP_PB_CAPTURE_FLIP (the Nth
FLIP_STALL; only reproducible if the boot path is bit-identical); the two are
mutually exclusive. The ms clock starts at the first pushbuffer method, not at
process start, and a --capture-ms run lasts at least MS/1000 + 15 s so the
write isn't cut off. Capture runs only the pad checkpoint and requires it:
combining with --only boot is an error.
The capture file must exist, and [PBCAP] wrote must come after WADCRC.BIN opens
(the level load), so a title-screen or movie frame is not accepted as a level
frame.

--gpu runs the checkpoints with RECOMP_PB_GPU=1: the pushbuffer goes to xemu's
PGRAPH (src/gpu, OpenGL renderer; RECOMP_PB_GPU_RENDERER=null for the null one)
instead of the software executor. The draw count then comes from its [XGPU]
stats line, which must also show no dropped object handles, and the renderer
must have come up. With OpenGL, flips must also have presented frames and one
of them must have been non-black. Not combinable with a capture (the
capture's reference frame is the software executor's).
"""

import argparse
import os
import re
import struct
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
    "RECOMP_APU_DSP_ACK": "auto",
    "RECOMP_HEAP_RECLAIM": "1",
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

# A level frame has ~195k non-black pixels; a black/loading frame ~11k of
# near-zero noise. This floor rejects the latter without needing an eyeball.
CAPTURE_MIN_NONBLACK = 30000

# With --capture-ms the pad run lasts at least T/1000 + this many seconds.
CAPTURE_TAIL_SECS = 15


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
    # 2.H.4b: on the GPU path (60 fps) the menus take presses faster and the accept
    # lands at step 7, so the files are anchored after step 5 (the first A, past the
    # title): that still rules out the attract loop's OPENING.SFD.
    Checkpoint("pad", 90, {"RECOMP_PAD_SCRIPT": PAD_SCRIPT + "," + PAD_SKIP_MOVIE}, 25000,
               ["MIDWAY.SFD", "SaveMeta.xbx", "GAMEDATA.WAH"], last_pad_step=9,
               files_after_step=(5, ["OPENING.SFD", "WADCRC.BIN"])),
]

RE_DRAWS = re.compile(r"^\[GPU\] draws (\d+) ", re.M)
RE_DSP_ACK = re.compile(r"\[APU\] DSP doorbell 0x[0-9A-Fa-f]+: command 0x[0-9A-Fa-f]+ acknowledged")
RE_PATH = re.compile(r"^\s*\[PATH\] (.+)$", re.M)
RE_WILD = re.compile(r"\[ICALL\] target 0x([0-9A-Fa-f]+) is not code")
RE_PAD = re.compile(r"PAD: t=\d+ ms step (\d+)")
RE_PAD_STEP = r"PAD: t=\d+ ms step %d "
RE_PBCAP_ARMED = re.compile(r"\[PBCAP\] armed at flip (\d+)")
RE_PBCAP_WROTE = re.compile(r"\[PBCAP\] wrote ")
RE_FLIPS = re.compile(r"\((\d+) flips\)")
RE_XGPU_UP = re.compile(r"\[XGPU\] renderer '([^']+)' up")
RE_XGPU_STATS = re.compile(r"^\[XGPU\] methods \d+, .*?draws (\d+), .*?bad handles (\d+)", re.M)
RE_XGPU_PRESENTS = re.compile(r"^\[XGPU\] methods .*?presents (\d+) \(.*?(\d+) non-black px\)", re.M)


def _path_pos(text, name):
    """End offset of the first [PATH] line whose path ends with name."""
    for m in RE_PATH.finditer(text):
        if m.group(1).lower().endswith(name.lower()):
            return m.end()
    return None


def _bmp_nonblack(path):
    """Count non-black pixels in a 24-bit bottom-up BMP, 0 on any error."""
    try:
        d = Path(path).read_bytes()
        if d[:2] != b"BM" or struct.unpack("<H", d[28:30])[0] != 24:
            return 0
        off = struct.unpack("<I", d[10:14])[0]
        w = struct.unpack("<i", d[18:22])[0]
        h = struct.unpack("<i", d[22:26])[0]
        stride = ((w * 3 + 3) // 4) * 4
        n = 0
        for y in range(abs(h)):
            row = off + (abs(h) - 1 - y) * stride
            for x in range(w):
                i = row + x * 3
                if d[i] or d[i + 1] or d[i + 2]:
                    n += 1
        return n
    except (OSError, struct.error, IndexError):   # IndexError: truncated file
        return 0


def run(cp, exe, game_dir, log, extra_env=None, secs=None):
    secs = secs or cp.secs
    env = dict(os.environ)
    env.update(RUN_ENV)
    env.update(cp.env)
    if extra_env:
        env.update(extra_env)
    env["RECOMP_WATCHDOG_SECS"] = str(secs)
    print(f"[{cp.name}] running {secs} s, log {log}", flush=True)
    start = time.monotonic()
    with open(log, "wb") as f:
        try:
            # From the exe's folder, like the run scripts.
            code = subprocess.run([str(exe), str(game_dir)], cwd=exe.parent, env=env,
                                  stdout=f, stderr=subprocess.STDOUT,
                                  timeout=secs + 60).returncode
        except subprocess.TimeoutExpired:
            code = None
    print(f"[{cp.name}] exit {code} after {time.monotonic() - start:.0f} s", flush=True)
    return code


def check(cp, code, text, capture_after=None, gpu=False):
    """Return a list of (ok, message). gpu: the run used RECOMP_PB_GPU=1."""
    results = []
    results.append((code in (0, 3), f"exit code {code} (0 or 3 = watchdog expected)"))
    crashes = text.count("[CRASH]")
    results.append((crashes == 0, f"{crashes} [CRASH]"))
    # xemu's PGRAPH keeps its asserts (abort() exits 3, like the watchdog).
    asserts = text.count("Assertion failed")
    results.append((asserts == 0, f"{asserts} failed asserts"))
    unresolved = sorted(set(re.findall(r"Failed to resolve VA (0x[0-9A-Fa-f]+)", text)))
    results.append((not unresolved, f"{len(unresolved)} unresolved ICALL targets "
                    + " ".join(unresolved[:10])))
    wild = {int(v, 16) for v in RE_WILD.findall(text)}
    bad = sorted(wild - KNOWN_WILD)
    results.append((not bad, f"{len(bad)} wild ICALL targets "
                    + " ".join(f"0x{v:08X}" for v in bad)))
    for v in sorted(wild & KNOWN_WILD):
        print(f"  note: known wild ICALL 0x{v:08X}")
    # 1.15e: the bump-only contiguous arena ran out before the movie after New
    # Game, which then stayed black. RECOMP_HEAP_RECLAIM=1 fixes it.
    exhausted = text.count("arena exhausted")
    results.append((exhausted == 0, f"{exhausted} [CONTIG] arena exhausted"))
    # RECOMP_APU_DSP_ACK=auto found DirectSound's doorbell; without the ack
    # the boot stalls in DSOUND init.
    results.append((bool(RE_DSP_ACK.search(text)), "DSP doorbell acknowledged"))
    if gpu:
        m = RE_XGPU_UP.search(text)
        results.append((bool(m), "xemu PGRAPH renderer up"
                        + (f" ({m.group(1)})" if m else "")))
        stats = [(int(d), int(b)) for d, b in RE_XGPU_STATS.findall(text)]
        draws = [d for d, _ in stats]
        bad = max((b for _, b in stats), default=0)
        results.append((bad == 0, f"{bad} object handles not in RAMHT"))
        if m and m.group(1) == "OpenGL":
            pres = [(int(p), int(nb)) for p, nb in RE_XGPU_PRESENTS.findall(text)]
            presents = max((p for p, _ in pres), default=0)
            nonblack = max((nb for _, nb in pres), default=0)
            results.append((presents > 0, f"{presents} frames presented"))
            results.append((nonblack > 0, f"a presented frame has {nonblack} non-black px"))
    else:
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
    if capture_after is not None:
        pos = _path_pos(text, capture_after)
        wrote = bool(RE_PBCAP_WROTE.search(text, pos)) if pos is not None else False
        results.append((wrote, f"[PBCAP] wrote after {capture_after}"))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game-dir", default=os.environ.get("MKSM_GAME_DIR",
                                                         str(PORT.parent / "game_files")))
    ap.add_argument("--exe", default=str(PORT / "build" / "Release" / "mksm_recomp.exe"))
    ap.add_argument("--log-dir", default=str(PORT.parent / "logs"))
    ap.add_argument("--only", choices=[c.name for c in CHECKPOINTS], action="append")
    cap = ap.add_mutually_exclusive_group()
    cap.add_argument("--capture-ms", type=int,
                     help="capture the pad frame at the first flip after this many ms")
    cap.add_argument("--capture-flip", type=int,
                     help="capture the pad frame at this FLIP_STALL number")
    ap.add_argument("--capture-path",
                    help="capture file path (default <log_dir>/pbcap_level_<N>.pbcap)")
    ap.add_argument("--gpu", action="store_true",
                    help="RECOMP_PB_GPU=1: run on xemu's PGRAPH, not the software executor")
    args = ap.parse_args()

    game_dir = Path(args.game_dir).resolve()
    exe = Path(args.exe).resolve()
    if not (game_dir / "default.xbe").is_file():
        print(f"SKIP: no default.xbe in {game_dir} (set MKSM_GAME_DIR or --game-dir)")
        return SKIP
    if not exe.is_file():
        print(f"FAIL: exe not found: {exe}")
        return 1
    log_dir = Path(args.log_dir).resolve()
    log_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")

    capture = args.capture_ms is not None or args.capture_flip is not None
    if capture and args.gpu:
        print("FAIL: --gpu can't be combined with a capture (its reference frame is the "
              "software executor's)")
        return 1
    capture_env = {}
    capture_path = None
    capture_secs = None
    if capture:
        if args.only and "pad" not in args.only:
            print("FAIL: --capture-ms/--capture-flip record a pad frame, so they "
                  "need the pad checkpoint (conflicts with --only)")
            return 1
        # The capture is a pad-run concern; don't also spend ~90 s on boot.
        args.only = ["pad"]
        tag = args.capture_ms if args.capture_ms is not None else args.capture_flip
        capture_path = Path(args.capture_path).resolve() if args.capture_path \
            else (log_dir / f"pbcap_level_{tag}.pbcap").resolve()
        capture_env["RECOMP_PB_CAPTURE"] = str(capture_path)
        if args.capture_ms is not None:
            capture_env["RECOMP_PB_CAPTURE_MS"] = str(args.capture_ms)
            # The capture clock starts a little after the process, and the
            # write needs one more flip (~250 ms in the level) plus ~288 MB of
            # copies: leave a tail so the watchdog doesn't cut it off.
            capture_secs = args.capture_ms // 1000 + CAPTURE_TAIL_SECS
        if args.capture_flip is not None:
            capture_env["RECOMP_PB_CAPTURE_FLIP"] = str(args.capture_flip)

    failed = []
    for cp in CHECKPOINTS:
        if args.only and cp.name not in args.only:
            continue
        log = (log_dir / f"smoke_{stamp}_{cp.name}.log").resolve()
        secs = max(cp.secs, capture_secs) if capture_secs else None
        extra = dict(capture_env)
        if args.gpu:
            extra["RECOMP_PB_GPU"] = "1"
        code = run(cp, exe, game_dir, log, extra_env=extra or None, secs=secs)
        text = log.read_text(encoding="utf-8", errors="replace")
        cap_after = "WADCRC.BIN" if (capture and cp.name == "pad") else None
        for ok, msg in check(cp, code, text, capture_after=cap_after, gpu=args.gpu):
            print(f"  {'ok  ' if ok else 'FAIL'} {msg}")
            if not ok:
                failed.append(f"{cp.name}: {msg}")
        if capture and cp.name == "pad":
            ref = Path(str(capture_path) + ".ref.bmp")
            for f in (capture_path, ref):
                ok = f.is_file()
                print(f"  {'ok  ' if ok else 'FAIL'} capture file {f.name}")
                if not ok:
                    failed.append(f"{cp.name}: capture file {f.name} missing")
            nb = _bmp_nonblack(ref) if ref.is_file() else 0
            ok = nb >= CAPTURE_MIN_NONBLACK
            print(f"  {'ok  ' if ok else 'FAIL'} capture frame non-black pixels "
                  f"{nb} (need >= {CAPTURE_MIN_NONBLACK})")
            if not ok:
                failed.append(f"{cp.name}: capture frame too dark ({nb} non-black pixels)")
            # If the capture never fired, this is the data to pick the next
            # target with: was it armed, and how far did the run actually get.
            armed = bool(RE_PBCAP_ARMED.search(text))
            wrote = bool(RE_PBCAP_WROTE.search(text))
            hi = max((int(x) for x in RE_FLIPS.findall(text)), default=0)
            if not wrote:
                print(f"  [capture] armed={armed} wrote={wrote} highest flip={hi} "
                      f"(target {tag})")
    if failed:
        print(f"FAILED ({len(failed)}):")
        for f in failed:
            print(f"  {f}")
        return 1
    print("PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
