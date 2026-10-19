"""Per-thread CPU profile of a headless pad run on Linux (perf), like RECOMP_PROF on Windows.

prof.c (RECOMP_PROF) is Win32 only. On Linux this runs the smoke test's pad checkpoint, attaches
`perf record` to the game for a window inside the level, reads every thread's CPU time from
/proc before and after the window, and prints per thread: CPU % of the window, the top exclusive
symbols, and the outermost frames of its stacks (which say what the thread is: POSIX threads
aren't named yet, so most show the process name).

    MKSM_EXE=~/mksm-build/port/mksm_recomp python3 tools/perf_profile.py --runs 3
    python3 tools/perf_profile.py --report ../logs/perfprof_X.data   # re-summarize a recording
    python3 tools/perf_profile.py --offcpu       # where guest main + the method thread wait

--offcpu records every context switch of guest main and the method thread instead (a stack at
each switch-out, the switch-in/out times): off-CPU time per blocking site (kernel wait | user
frames), per second of the window, and each thread's run-queue delay from /proc schedstat. Needs
perf_event_paranoid <= 1 (kernel stacks; -1 is what the Deck uses), no tracefs/root.

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


def perf_script(data, offcpu=False):
    fields = ["--show-switch-events", "-F", "tid,time,event,ip,sym"] if offcpu else \
        ["-F", "comm,tid,ip,sym"]
    return subprocess.run(["perf", "script", "-i", str(data), *fields],
                          capture_output=True, text=True, errors="replace").stdout


SWITCH_HEAD_RE = re.compile(r"\s*(\d+)\s+([\d.]+):\s+(\S+)(?:\s+(IN|OUT))?")
WAIT_FRAMES = {"schedule", "__schedule", "schedule_hrtimeout_range", "[unknown]"}


def offcpu_site(frames, depth=6):
    """Blocking site of a switch-out stack (leaf first, (addr, sym)): the kernel frame below the
    scheduler (futex_do_wait, schedule_timeout, ...) | the first `depth` named user frames."""
    kern = [s for a, s in frames if len(a) == 16 and a.startswith("ffff")]
    user = [s for a, s in frames if not (len(a) == 16 and a.startswith("ffff"))]
    k = next((s for s in kern if s not in WAIT_FRAMES), kern[0] if kern else "?")
    named = [s for s in user if s != "[unknown]"]
    return f"{k} | " + " < ".join(named[:depth])


def parse_offcpu_script(text, depth=6):
    """`perf script --show-switch-events -F tid,time,event,ip,sym` of a `-e context-switches -c 1
    --switch-events` recording → ({tid: {"site": Counter(site: s off CPU), "n": Counter(site),
    "sec": Counter(second of the window: s off CPU)}}, span s). Each switch-out sample carries the
    stack; its off-CPU time runs from the OUT record to the thread's next IN record."""
    events, cur = [], None
    for line in text.splitlines():
        if line.startswith("\t"):
            if cur is not None:
                parts = line.split(None, 1)
                cur[3].append((parts[0], parts[1].strip() if len(parts) > 1 else "[unknown]"))
            continue
        m = SWITCH_HEAD_RE.match(line)
        if m:
            kind = m.group(4) or m.group(3)
            cur = (int(m.group(1)), float(m.group(2)), kind, [])
            events.append(cur)
    if not events:
        return {}, 0.0
    t0 = min(e[1] for e in events)
    span = max(e[1] for e in events) - t0
    out = collections.defaultdict(lambda: {"site": collections.Counter(),
                                           "n": collections.Counter(),
                                           "sec": collections.Counter()})
    site, t_out = {}, {}
    for tid, t, kind, frames in events:
        if kind == "context-switches:":
            site[tid] = offcpu_site(frames, depth)
        elif kind == "OUT":
            t_out[tid] = t
        elif kind == "IN" and tid in t_out:
            s, d = site.pop(tid, None), t - t_out[tid]
            if s is not None:
                o = out[tid]
                o["site"][s] += d
                o["n"][s] += 1
                o["sec"][int(t_out[tid] - t0)] += d
            del t_out[tid]
    return dict(out), span


def read_schedstat(pid):
    """{tid: (run ns, run-queue wait ns)} from /proc/<pid>/task/*/schedstat."""
    out = {}
    for d in Path(f"/proc/{pid}/task").iterdir():
        try:
            run, wait, _ = (int(x) for x in (d / "schedstat").read_text().split())
            out[int(d.name)] = (run, wait)
        except (OSError, ValueError):
            pass
    return out


def offcpu_summary(offcpu, span, shares, runq, secs, top=8):
    lines = [f"off-CPU window {span:.1f} s (context switches of the recorded threads)"]
    comm = {tid: c for tid, c, _ in shares}
    cpu = {tid: p for tid, _, p in shares}
    for tid, o in sorted(offcpu.items(), key=lambda kv: -cpu.get(kv[0], 0)):
        total = sum(o["site"].values())
        rq = runq.get(tid)
        lines.append(f"thread {tid} ({comm.get(tid, '?')}): CPU {cpu.get(tid, 0):.0f}%, off CPU "
                     f"{100 * total / span:.0f}%" + (f", run queue {rq:.1f}%" if rq is not None
                                                      else ""))
        for s, d in o["site"].most_common(top):
            lines.append(f"  {100 * d / span:5.1f}%  n={o['n'][s]:<6d} avg "
                         f"{1e6 * d / o['n'][s]:6.0f} us  {s}")
        lines.append("  off CPU per s: " + " ".join(f"{100 * o['sec'][i]:.0f}"
                                                    for i in range(int(span))))
    return lines


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
    before, rq0 = snapshot(pid), read_schedstat(pid)
    t_in = time.monotonic()
    if args.offcpu:
        # Every switch of guest main (pid) and the method thread (the first thread the game
        # starts: pid + 1, as in 2.7b2-2.7c7's runs), with its stack. All threads would be
        # ~20k switches/s with an 8 KiB stack copy each.
        what = ["-e", "context-switches", "-c", "1", "--switch-events", "--call-graph",
                "dwarf,8192", "-t", f"{pid},{pid + 1}"]
    else:
        what = ["-p", str(pid), "-F", str(args.freq), "--call-graph", "dwarf,16384"]
    rec = subprocess.run(["perf", "record", "-q", *what, "-o", str(data), "--", "sleep",
                          str(args.secs)], capture_output=True, text=True)
    after, rq1 = snapshot(pid), read_schedstat(pid)
    secs = time.monotonic() - t_in
    runq = {tid: 100.0 * (w - rq0.get(tid, (0, 0))[1]) / 1e9 / secs
            for tid, (_, w) in rq1.items()}
    # perf's DWARF unwinder fails with "No such process" for most samples (perf 7.1 + libdw on
    # Nobara: 1 thread in 12 unwound in 2.7b1's runs; less after the game exits), so the call
    # chains are read while the game still runs (the watchdog ends it at 90 s). The self
    # symbols come from perf report and don't need unwinding.
    script = perf_script(data, args.offcpu)
    smoke.wait()
    reader.join(5)
    smoke_out = "".join(out_lines)
    if rec.returncode:
        print(rec.stderr, file=sys.stderr)
    m = SMOKE_LOG_RE.search(smoke_out)
    log = m.group(1) if m else None
    passed = "PASSED" in smoke_out
    return data, script, cpu_shares(before, after, secs), secs, log, passed, runq


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
    ap.add_argument("--offcpu", action="store_true",
                    help="record where guest main and the method thread wait (context switches) "
                         "instead of CPU samples")
    ap.add_argument("--depth", type=int, default=6, help="--offcpu: user frames per site")
    args = ap.parse_args(argv)

    if args.report and args.offcpu:
        offcpu, span = parse_offcpu_script(perf_script(args.report, True), args.depth)
        print("\n".join(offcpu_summary(offcpu, span, [], {}, span, args.top)))
        return 0
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
        data, script, shares, secs, log, passed, runq = one_run(args, exe, stamp)
        lines = [f"smoke log {log} ({'PASSED' if passed else 'NOT passed'})",
                 "level fps: " + (" ".join(f"{x:.1f}" for x in level_fps(log)) if log else "-")]
        if args.offcpu:
            offcpu, span = parse_offcpu_script(script, args.depth)
            lines += offcpu_summary(offcpu, span, shares, runq, secs, args.top)
            threads = {}
        else:
            threads = parse_perf_script(script)
            flat = parse_perf_report(perf_report(data))
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
