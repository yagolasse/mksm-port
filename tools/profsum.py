"""Summarize RECOMP_PROF profiles and level fps from one or more run logs.

Per log: frame rate per [XGPU] interval (flips / the interval the line ends
with; 5 s for logs from before that field), the intervals inside the profile window, and per thread the CPU
share of the window and the top exclusive functions. With several logs it
also prints the median per thread name (the thread id differs per run; the
name, from SetThreadDescription or the start symbol, does not) and the median
total in cores.

    python tools/profsum.py ../logs/smoke_A_pad.log ../logs/smoke_B_pad.log
    python tools/profsum.py --top 5 --min-draws 300 --inclusive LOG...

--min-draws keeps only intervals with at least that many draws per flip (the
level draws ~500-600 per flip, menus and movies < 150), so "level fps" can be
read off a pad run that also contains the boot.
"""

import argparse
import re
import statistics
import sys

XGPU_RE = re.compile(r"\[XGPU\] methods \d+.*?draws (\d+), flips (\d+).*?(?:px\), ([\d.]+) s)?$")
THREAD_RE = re.compile(
    r"\[PROF\] thread \d+ \((.*)\): \d+ samples, CPU (\d+) ms = (\d+)% of the window")
SAMPLING_RE = re.compile(r"\[PROF\] sampling \d+ threads")
DONE_RE = re.compile(r"\[PROF\] \d+ samples over (\d+) ms")
FUNC_RE = re.compile(r"\s+([\d.]+)%\s+\d+\s+(\S+)")
INTERVAL_S = 5.0


def parse(text, top=5):
    """Returns dict: intervals [(fps, draws_per_flip, in_window)], threads
    [(name, cpu_pct, cpu_ms, exclusive [(func, pct)...], inclusive [...])],
    window_ms."""
    lines = text.splitlines()
    intervals, threads = [], []
    prev = None
    state = "before"  # before / in / after the profile window
    window_ms = None
    for i, line in enumerate(lines):
        if SAMPLING_RE.search(line):
            state = "in"
            prev = None if prev is None else (prev[0], prev[1], False)
            continue
        m = DONE_RE.search(line)
        if m:
            window_ms = int(m.group(1))
            state = "after"
            continue
        m = XGPU_RE.search(line)
        if m:
            draws, flips = int(m.group(1)), int(m.group(2))
            # Newer lines end with the real interval (later than 5 s when few methods come).
            secs = float(m.group(3)) if m.group(3) and float(m.group(3)) > 0 else INTERVAL_S
            if prev is not None and flips > prev[1]:
                n = flips - prev[1]
                # An interval counts as profiled only if it began and ended in the window.
                inside = state == "in" and prev[2]
                intervals.append((n / secs, (draws - prev[0]) // n, inside))
            prev = (draws, flips, state == "in")
            continue
        m = THREAD_RE.match(line)
        if m:
            funcs, incl = [], []
            cur = funcs
            j = i + 1
            while j < len(lines) and not lines[j].startswith("[PROF]"):
                if lines[j].strip().startswith("inclusive"):
                    cur = incl
                f = FUNC_RE.match(lines[j])
                if f and len(cur) < top:
                    cur.append((f.group(2), float(f.group(1))))
                j += 1
            threads.append((m.group(1), int(m.group(3)), int(m.group(2)), funcs, incl))
    return {"intervals": intervals, "threads": threads, "window_ms": window_ms}


def level_fps(intervals, min_draws):
    return [fps for fps, dpf, _ in intervals if dpf >= min_draws]


def short(func):
    return func.replace("mksm_recomp!", "")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--top", type=int, default=5, help="exclusive functions per thread")
    ap.add_argument("--min-draws", type=int, default=300,
                    help="draws per flip for an interval to count as level")
    ap.add_argument("--inclusive", action="store_true", help="also print the top inclusive")
    args = ap.parse_args(argv)

    per_name = {}
    totals = []
    for path in args.logs:
        with open(path, encoding="utf-8", errors="replace") as f:
            r = parse(f.read(), args.top)
        print(f"== {path}")
        lv = level_fps(r["intervals"], args.min_draws)
        win = [fps for fps, dpf, inside in r["intervals"] if inside and dpf >= args.min_draws]
        print("  level fps: " + (" ".join(f"{x:.1f}" for x in lv) or "-")
              + ("  | in the profile window: " + " ".join(f"{x:.1f}" for x in win) if win else ""))
        total = 0
        for name, pct, ms, funcs, incl in r["threads"]:
            total += pct
            per_name.setdefault(name, []).append(pct)
            print(f"  {pct:4d}%  {name}")
            print("    excl " + "; ".join(f"{short(fn)} {p:.1f}" for fn, p in funcs))
            if args.inclusive:
                print("    incl " + "; ".join(f"{short(fn)} {p:.1f}" for fn, p in incl))
        if r["threads"]:
            totals.append(total)
            print(f"  total: {total}% = {total / 100:.2f} cores")
    if len(args.logs) > 1 and per_name:
        n = len(args.logs)
        print(f"== median of {n} logs (threads missing from a log count as < 2%)")
        rows = sorted(per_name.items(), key=lambda kv: -statistics.median(kv[1]))
        for name, pcts in rows:
            print(f"  {statistics.median(pcts):6.1f}%  ({'/'.join(map(str, pcts))})  {name}")
        if totals:
            print(f"  total: median {statistics.median(totals) / 100:.2f} cores "
                  f"({'/'.join(f'{t / 100:.2f}' for t in totals)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
