"""Per-thread CPU profile of a headless pad run on Linux (perf), like RECOMP_PROF on Windows.

prof.c (RECOMP_PROF) is Win32 only. On Linux this runs the smoke test's pad checkpoint, attaches
`perf record` to the game for a window inside the level, reads every thread's CPU time from
/proc before and after the window, and prints per thread: CPU % of the window, the top exclusive
symbols, and the outermost frames of its stacks (which say what the thread is: POSIX threads
aren't named yet, so most show the process name).

    MKSM_EXE=~/mksm-build/port/mksm_recomp python3 tools/perf_profile.py --runs 3
    python3 tools/perf_profile.py --report ../logs/perfprof_X.data   # re-summarize a recording

Defaults match 2.7a's Windows window (RECOMP_PROF=62000,15000: from 62 s, 15 s). Needs
perf_event_paranoid <= 2 (user-space samples of your own process); the call graphs use DWARF
unwinding (the build has no frame pointers). Output per run: <log-dir>/perfprof_<stamp>.data
(perf), .txt (this summary); the smoke log as usual. The machine should be idle.
Call chains (root/incl lines) are best effort: perf's DWARF unwinding fails for most threads
here; the CPU % (from /proc) and the self symbols (perf report) are the reliable part.
"""

import argparse
import collections
import os
import re
import shlex
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profsum  # noqa: E402

PORT = Path(__file__).resolve().parent.parent
LEVEL_MIN_DRAWS = 300
# Frames that say nothing about what a thread is (thread start glue, libc).
GLUE = {"start_thread", "clone3", "__clone", "__clone3", "__GI___clone3", "[unknown]",
        "thread_trampoline", "_start", "__libc_start_main", "__libc_start_call_main"}
SMOKE_LOG_RE = re.compile(r"\[pad\] running \d+ s, log (\S+)")


def parse_task_stat(text):
    """/proc/<pid>/task/<tid>/stat → (comm, utime + stime in clock ticks)."""
    lp, rp = text.index("("), text.rindex(")")
    comm = text[lp + 1:rp]
    rest = text[rp + 2:].split()
    # rest[0] is field 3 (state); utime/stime are fields 14/15.
    return comm, int(rest[11]) + int(rest[12])


def snapshot(pid):
    """{tid: (comm, cpu ticks)} for every thread of pid; threads that exit meanwhile are skipped."""
    out = {}
    base = Path(f"/proc/{pid}/task")
    for d in base.iterdir():
        try:
            out[int(d.name)] = parse_task_stat((d / "stat").read_text())
        except (OSError, ValueError):
            pass
    return out


def cpu_shares(before, after, secs, hz=None):
    """[(tid, comm, cpu %)] over the window, highest first. A thread born in the window counts
    from 0."""
    hz = hz or os.sysconf("SC_CLK_TCK")
    rows = []
    for tid, (comm, ticks) in after.items():
        t0 = before.get(tid, (comm, 0))[1]
        rows.append((tid, comm, 100.0 * (ticks - t0) / hz / secs))
    rows.sort(key=lambda r: -r[2])
    return rows


def parse_perf_script(text):
    """`perf script -F comm,tid,ip,sym` with call chains → {tid: {"samples": n,
    "self": Counter(sym), "incl": Counter(sym), "root": Counter(outermost frames)}}."""
    threads = collections.defaultdict(lambda: {"samples": 0, "self": collections.Counter(),
                                               "incl": collections.Counter(),
                                               "root": collections.Counter()})
    tid, frames = None, []

    def flush():
        if tid is None:
            return
        t = threads[tid]
        t["samples"] += 1
        if frames:
            t["self"][frames[0]] += 1
            for f in set(frames):
                t["incl"][f] += 1
            useful = [f for f in frames if f not in GLUE]
            if useful:
                t["root"][" < ".join(useful[-3:][::-1])] += 1

    for line in text.splitlines():
        if not line.strip():
            flush()
            tid, frames = None, []
        elif line[0] in " \t":
            parts = line.split(None, 1)
            sym = parts[1] if len(parts) > 1 else "[unknown]"
            frames.append(sym.split(" (")[0].split("+0x")[0].strip() or "[unknown]")
        else:
            flush()
            m = re.match(r"(.*?)\s+(\d+)\s*$", line.split(" [")[0]) or \
                re.match(r"(.*?)\s+(\d+)\b", line)
            tid, frames = (int(m.group(2)) if m else -1), []
    flush()
    return dict(threads)


REPORT_RE = re.compile(r"^\s*([\d.]+)%\s*;\s*(\d+):(.*?)\s*;\[.\]\s*(.+?)\s*$")


def parse_perf_report(text):
    """`perf report --sort pid,sym -t ';'` → {tid: Counter(sym: % of all samples)}. Unlike perf
    script it keeps the samples whose DWARF unwind failed (most of them in the GL driver)."""
    out = collections.defaultdict(collections.Counter)
    for line in text.splitlines():
        m = REPORT_RE.match(line)
        if m:
            out[int(m.group(2))][m.group(4).split("@@")[0]] += float(m.group(1))
    return dict(out)


def perf_report(data):
    return subprocess.run(["perf", "report", "-i", str(data), "--no-children", "-g", "none",
                           "--sort", "pid,sym", "--stdio", "-t", ";"],
                          capture_output=True, text=True, errors="replace").stdout


def perf_script(data):
    return subprocess.run(["perf", "script", "-i", str(data), "-F", "comm,tid,ip,sym"],
                          capture_output=True, text=True, errors="replace").stdout


def summarize(shares, threads, flat, secs, top=6, min_pct=2.0):
    lines = [f"window {secs:.0f} s; total {sum(r[2] for r in shares) / 100:.2f} cores"]
    for tid, comm, pct in shares:
        if pct < min_pct:
            continue
        lines.append(f"thread {tid} ({comm}): CPU {pct:.0f}%")
        f = flat.get(tid)
        n = sum(f.values()) if f else 0
        if n:
            lines.append("  self: " + ", ".join(f"{short(s)} {100 * c / n:.1f}"
                                                for s, c in f.most_common(top)))
        t = threads.get(tid)
        # Only the samples DWARF could unwind (most of the GL driver's can't).
        n = sum(t["root"].values()) if t else 0
        if not n:
            continue
        root = t["root"].most_common(1)[0]
        lines.append(f"  root: {root[0]} ({100 * root[1] / n:.0f}% of {n} unwound)")
        incl = [(s, c) for s, c in t["incl"].most_common(top + 6) if s not in GLUE][:top]
        lines.append("  incl: " + ", ".join(f"{s} {100 * c / n:.0f}" for s, c in incl))
    small = [r for r in shares if r[2] < min_pct]
    lines.append(f"{len(small)} threads below {min_pct:.0f}% "
                 f"(together {sum(r[2] for r in small):.0f}%)")
    return lines


def rank_keys(shares, threads):
    """[(key, root, cpu %)]: thread ids differ per run, so a thread is keyed by its name and its
    rank among the threads of that name (by CPU); root = its usual outermost frames, if unwound."""
    seen = collections.Counter()
    out = []
    for tid, comm, pct in shares:
        seen[comm] += 1
        root = threads.get(tid, {}).get("root")
        out.append((f"{comm} #{seen[comm]}", root.most_common(1)[0][0] if root else "", pct))
    return out


def short(sym, n=48):
    """C++ symbols from Mesa carry whole template argument lists."""
    sym = sym.split("(")[0] if "(" in sym and not sym.startswith("(") else sym
    return sym if len(sym) <= n else sym[:n - 3] + "..."


def level_fps(log):
    r = profsum.parse(Path(log).read_text(errors="replace"))
    return profsum.level_fps(r["intervals"], LEVEL_MIN_DRAWS)


def find_game(exe_name, after):
    """Newest process named exe_name started after `after` (monotonic-ish: its pid is new)."""
    out = subprocess.run(["pgrep", "-n", "-x", exe_name[:15]], capture_output=True, text=True)
    pid = out.stdout.strip()
    return int(pid) if pid and int(pid) not in after else None


def one_run(args, exe, stamp):
    data = Path(args.log_dir) / f"perfprof_{stamp}.data"
    old = set(int(p) for p in subprocess.run(["pgrep", "-x", exe.name[:15]], capture_output=True,
                                             text=True).stdout.split())
    env = dict(os.environ, MKSM_EXE=str(exe))
    smoke = subprocess.Popen([sys.executable, str(PORT / "tools" / "smoke_test.py"), "--only",
                              "pad", "--log-dir", args.log_dir], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out_lines = []
    reader = threading.Thread(target=lambda: out_lines.extend(smoke.stdout), daemon=True)
    reader.start()
    pid, t0 = None, time.monotonic()
    while pid is None and time.monotonic() - t0 < 30:
        time.sleep(0.2)
        pid = find_game(exe.name, old)
    if pid is None:
        smoke.wait()
        raise SystemExit("game process not found")
    time.sleep(max(0.0, args.start - (time.monotonic() - t0)))
    before = snapshot(pid)
    t_in = time.monotonic()
    rec = subprocess.run(["perf", "record", "-q", "-p", str(pid), "-F", str(args.freq),
                          "--call-graph", "dwarf,16384", "-o", str(data), "--", "sleep",
                          str(args.secs)], capture_output=True, text=True)
    after = snapshot(pid)
    secs = time.monotonic() - t_in
    # perf's DWARF unwinder fails with "No such process" for most samples (perf 7.1 + libdw on
    # Nobara: 1 thread in 12 unwound in 2.7b1's runs; less after the game exits), so the call
    # chains are read while the game still runs (the watchdog ends it at 90 s). The self
    # symbols come from perf report and don't need unwinding.
    script = perf_script(data)
    smoke.wait()
    reader.join(5)
    smoke_out = "".join(out_lines)
    if rec.returncode:
        print(rec.stderr, file=sys.stderr)
    m = SMOKE_LOG_RE.search(smoke_out)
    log = m.group(1) if m else None
    passed = "PASSED" in smoke_out
    return data, script, cpu_shares(before, after, secs), secs, log, passed


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", default=os.environ.get("MKSM_EXE",
                                                    str(PORT / "build" / "mksm_recomp")))
    ap.add_argument("--log-dir", default=str(PORT.parent / "logs"))
    ap.add_argument("--start", type=float, default=62.0, help="window start, s after launch")
    ap.add_argument("--secs", type=float, default=15.0, help="window length, s")
    ap.add_argument("--freq", type=int, default=499, help="samples per second per thread")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--top", type=int, default=6)
    ap.add_argument("--report", metavar="DATA", help="only summarize a recording (no CPU %%)")
    args = ap.parse_args(argv)

    if args.report:
        threads = parse_perf_script(perf_script(args.report))
        flat = parse_perf_report(perf_report(args.report))
        shares = [(tid, "?", 100.0 * t["samples"] / args.freq / args.secs)
                  for tid, t in sorted(threads.items(), key=lambda kv: -kv[1]["samples"])]
        print("\n".join(summarize(shares, threads, flat, args.secs, args.top)))
        return 0

    exe = Path(args.exe).expanduser().resolve()
    totals, per_thread, labels = [], collections.defaultdict(list), {}
    for i in range(args.runs):
        stamp = time.strftime("%Y%m%d_%H%M%S")
        print(f"run {i + 1}/{args.runs}: {shlex.quote(str(exe))}", flush=True)
        data, script, shares, secs, log, passed = one_run(args, exe, stamp)
        threads = parse_perf_script(script)
        flat = parse_perf_report(perf_report(data))
        lines = [f"smoke log {log} ({'PASSED' if passed else 'NOT passed'})",
                 "level fps: " + (" ".join(f"{x:.1f}" for x in level_fps(log)) if log else "-")]
        lines += summarize(shares, threads, flat, secs, args.top)
        txt = Path(args.log_dir) / f"perfprof_{stamp}.txt"
        txt.write_text("\n".join(lines) + "\n")
        print("\n".join(lines) + f"\n(summary {txt})\n", flush=True)
        totals.append(sum(r[2] for r in shares) / 100)
        for key, root, pct in rank_keys(shares, threads):
            per_thread[key].append(pct)
            labels[key] = labels.get(key) or root
    if args.runs > 1:
        print(f"median total {statistics.median(totals):.2f} cores ("
              + " / ".join(f"{t:.2f}" for t in totals) + ")")
        for key, v in sorted(per_thread.items(), key=lambda kv: -statistics.median(kv[1])):
            if statistics.median(v) >= 2:
                print(f"  {statistics.median(v):5.0f}%  ({len(v)} runs)  {key}  {labels[key]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
