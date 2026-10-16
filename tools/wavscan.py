"""Scan a recorded audio output (RECOMP_AUDIO_WAV; RECOMP_APU_WAV before toolkit v0.14) for glitches.

The APU sends one 256-sample block every ~5.33 ms; this looks at the same
blocks and flags the kinds of damage an emulated sound chip produces:

  noisy    a block with signal whose sample-to-sample difference is close to
           its level (white-noise-like: garbage sample data reads as this)
  click    one step far larger than the block's typical step (a discontinuity:
           a ring buffer read at the wrong place, a voice cut mid-wave)
  repeat   a non-silent block identical to the one before (a resent block)
  dropout  a silent block between two loud ones (a missed block)
  clip     samples at full scale

Consecutive flagged blocks are merged into events. The report lists them
with their times, then a per-second table, then how regular the event
spacing is (a buzz that repeats at a fixed rate shows up as one interval).

    python tools/wavscan.py out.wav [--top 40] [--seconds]
    python tools/wavscan.py out.wav --excerpt 61.5,3 clip.wav

Stdlib only (wave + array); a 3-minute file takes a few seconds.
"""

import argparse
import array
import math
import sys
import wave

BLOCK = 256

# Thresholds, tuned on synthetic signals (tests/test_wavscan.py). Medians, so
# one spike doesn't hide itself: median |step| / median |sample| is ~1.2 for
# uniform noise and 2*pi*f/rate for a sine (0.5 at 4 kHz).
NOISY_RATIO = 0.8
NOISY_MIN_RMS = 200.0
CLICK_FACTOR = 12.0
CLICK_MIN_STEP = 4000
LOUD_RMS = 500.0
FULL_SCALE = 32767


def read_wav(path):
    """(rate, channels, interleaved int16 array)."""
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("%s: not 16-bit PCM" % path)
        rate, ch = w.getframerate(), w.getnchannels()
        data = array.array("h")
        data.frombytes(w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        data.byteswap()
    return rate, ch, data


def block_stats(data, ch, start, n):
    """Level and step statistics of n frames from frame `start`."""
    lo, hi = start * ch, (start + n) * ch
    seg = data[lo:hi]
    mags = []
    steps = []
    for c in range(ch):
        x = seg[c::ch]
        mags.extend(abs(v) for v in x)
        steps.extend(abs(b - a) for a, b in zip(x, x[1:]))
    count = len(mags) or 1
    mags_sorted = sorted(mags)
    steps_sorted = sorted(steps) or [0]
    return {
        "rms": math.sqrt(sum(v * v for v in mags) / count),
        "peak": mags_sorted[-1] if mags else 0,
        "med": mags_sorted[len(mags_sorted) // 2] if mags else 0,
        "med_step": steps_sorted[len(steps_sorted) // 2],
        "max_step": steps_sorted[-1],
        "clipped": sum(1 for v in mags if v >= FULL_SCALE),
        "raw": seg.tobytes(),
    }


def classify(blocks):
    """Per-block list of flag sets."""
    flags = []
    for i, b in enumerate(blocks):
        f = set()
        if b["rms"] >= NOISY_MIN_RMS and b["med_step"] >= NOISY_RATIO * b["med"]:
            f.add("noisy")
        typical = b["med_step"] if b["med_step"] > 1 else 1
        if b["max_step"] >= CLICK_MIN_STEP and b["max_step"] >= CLICK_FACTOR * typical:
            f.add("click")
        if b["clipped"]:
            f.add("clip")
        if i and b["peak"] > 0 and b["raw"] == blocks[i - 1]["raw"]:
            f.add("repeat")
        if 0 < i < len(blocks) - 1 and b["peak"] == 0 and \
                blocks[i - 1]["rms"] >= LOUD_RMS and blocks[i + 1]["rms"] >= LOUD_RMS:
            f.add("dropout")
        flags.append(f)
    return flags


def events(flags, rate):
    """Merge runs of flagged blocks: [(t_start, t_len, kinds, first, last)]."""
    out = []
    i = 0
    while i < len(flags):
        if not flags[i]:
            i += 1
            continue
        j = i
        kinds = set()
        while j < len(flags) and flags[j]:
            kinds |= flags[j]
            j += 1
        out.append((i * BLOCK / rate, (j - i) * BLOCK / rate, kinds, i, j - 1))
        i = j
    return out


def scan(path):
    rate, ch, data = read_wav(path)
    nframes = len(data) // ch
    blocks = [block_stats(data, ch, s, min(BLOCK, nframes - s))
              for s in range(0, nframes, BLOCK)]
    flags = classify(blocks)
    return rate, ch, nframes, blocks, flags, events(flags, rate)


def interval_histogram(evs, rate):
    """Most common gaps between event starts, in blocks."""
    gaps = {}
    for a, b in zip(evs, evs[1:]):
        g = b[3] - a[3]
        gaps[g] = gaps.get(g, 0) + 1
    return sorted(gaps.items(), key=lambda kv: -kv[1])


def report(path, top, seconds, out=sys.stdout):
    rate, ch, nframes, blocks, flags, evs = scan(path)
    p = lambda *a: print(*a, file=out)
    p("%s: %.1f s, %d Hz, %d ch, %d blocks" % (path, nframes / rate, rate, ch, len(blocks)))
    totals = {}
    for f in flags:
        for k in f:
            totals[k] = totals.get(k, 0) + 1
    p("flagged blocks: " + (", ".join("%s %d" % kv for kv in sorted(totals.items())) or "none"))
    p("events: %d" % len(evs))
    for t, ln, kinds, a, b in sorted(evs, key=lambda e: -(e[4] - e[3]))[:top]:
        worst = max(blocks[a:b + 1], key=lambda x: x["rms"])
        p("  t=%8.3f s  len=%6.1f ms  %-24s rms=%6.0f step/level=%.2f peak=%d max_step=%d"
          % (t, ln * 1000, ",".join(sorted(kinds)), worst["rms"],
             worst["med_step"] / worst["med"] if worst["med"] else 0, worst["peak"],
             max(x["max_step"] for x in blocks[a:b + 1])))
    if len(evs) > 2:
        hist = interval_histogram(evs, rate)[:5]
        p("event spacing (blocks of %.2f ms: count): %s" % (
            BLOCK * 1000 / rate, ", ".join("%d: %d" % kv for kv in hist)))
    if seconds:
        per = max(1, rate // BLOCK)
        p("second  rms   peak  noisy click repeat dropout clip")
        for s in range(0, len(blocks), per):
            bs, fs = blocks[s:s + per], flags[s:s + per]
            rms = math.sqrt(sum(b["rms"] ** 2 for b in bs) / len(bs))
            cnt = lambda k: sum(1 for f in fs if k in f)
            p("%6d %5.0f %6d %5d %5d %6d %7d %4d" % (
                s // per, rms, max(b["peak"] for b in bs), cnt("noisy"), cnt("click"),
                cnt("repeat"), cnt("dropout"), cnt("clip")))
    return evs


def excerpt(path, start_s, dur_s, dst):
    with wave.open(path, "rb") as w:
        rate = w.getframerate()
        w.setpos(min(w.getnframes(), int(start_s * rate)))
        frames = w.readframes(int(dur_s * rate))
        params = w.getparams()
    with wave.open(dst, "wb") as o:
        o.setparams(params)
        o.writeframes(frames)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("wav")
    ap.add_argument("--top", type=int, default=40, help="events to list (longest first)")
    ap.add_argument("--seconds", action="store_true", help="per-second table")
    ap.add_argument("--excerpt", metavar="START,DUR", help="write a clip instead of scanning")
    ap.add_argument("out", nargs="?", help="clip path for --excerpt")
    a = ap.parse_args(argv)
    if a.excerpt:
        if not a.out:
            ap.error("--excerpt needs an output path")
        s, d = (float(x) for x in a.excerpt.split(","))
        excerpt(a.wav, s, d, a.out)
        return 0
    report(a.wav, a.top, a.seconds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
