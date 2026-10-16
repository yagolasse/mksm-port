"""Analyse a per-voice source dump (RECOMP_APU_VOICE_DUMP).

The APU writes one record per batch of source samples a voice fetches:
    u32 vp_frame, u16 voice, u16 count, u32 cbo (before the fetch),
    count x (s16 L, s16 R)
in fetch order (little endian). vp_frame counts VP frames (32 output
samples, 1500 per second).

For each voice this reports how much it played and, for voices that come
back to the same buffer offsets (loops and rings the game refills), how
often a stretch played again is identical to what was played at those
offsets the lap before. A ring the game keeps refilling should never repeat;
a repeated stretch there means the voice read old data (the refill was late
or written somewhere else). A short loop of fixed data repeats by design.

    python tools/voicedump.py dump.bin                 # per-voice summary
    python tools/voicedump.py dump.bin --voice 50      # that voice's stale runs
    python tools/voicedump.py dump.bin --wav 50 v50.wav [--from S --to S]

Toolkit v0.14+ writes a different dump: raw s16 stereo 48 kHz, no headers,
one voice per file (RECOMP_APU_VOICE_DUMP=<voice> -> voice_dump.raw, or =all
-> <RECOMP_APU_VOICE_DUMP_FILE>.<voice>.raw), samples after the pitch
resampler and before gain. It has no buffer offsets, so the check above
can't run on it. Raw mode (a .raw file, or --raw) instead reports per file
the duration, peak, RMS and zero-crossing rate (music ~0.1-0.2, white noise
~0.5), and stretches that are an exact copy of earlier output (a stale ring
read at pitch 1.0; a resampled voice's repeats aren't sample-exact, and a
short loop of fixed data repeats by design):

    python tools/voicedump.py voice_dump.*.raw         # one line per file
    python tools/voicedump.py v.50.raw --voice 0       # its copied stretches
    python tools/voicedump.py v.50.raw --wav 0 v50.wav # (the voice is ignored)

Stdlib only.
"""

import argparse
import array
import os
import struct
import sys
import wave

HDR = struct.Struct('<IHHI')
VP_FRAMES_PER_S = 1500.0
MIN_RUN = 64        # identical samples in a row to count as a repeat
MIN_LEVEL = 64      # ... with at least this peak (silence repeats trivially)


def read_records(path):
    """Yield (vp_frame, voice, cbo, samples) with samples an array('h') L,R,..."""
    with open(path, 'rb') as f:
        data = f.read()
    pos = 0
    n = len(data)
    while pos + HDR.size <= n:
        frame, voice, count, cbo = HDR.unpack_from(data, pos)
        pos += HDR.size
        end = pos + count * 4
        if end > n:
            break           # truncated last record (run killed mid-write)
        smp = array.array('h')
        smp.frombytes(data[pos:end])
        if sys.byteorder != 'little':
            smp.byteswap()
        pos = end
        yield frame, voice, cbo, smp


class VoiceState:
    def __init__(self):
        self.samples = 0
        self.first = None
        self.last = None
        self.revisits = 0       # samples played at an offset seen before
        self.same = 0           # ... in identical runs of >= MIN_RUN
        self.runs = []          # (vp_frame, cbo_start, length, peak)
        self.prev = {}          # offset -> (L, R) last played there
        self.run = None         # [frame, cbo_start, length, peak] in progress

    def end_run(self):
        r = self.run
        self.run = None
        if r and r[2] >= MIN_RUN and r[3] >= MIN_LEVEL:
            self.same += r[2]
            self.runs.append(tuple(r))

    def feed(self, frame, cbo, smp):
        if self.first is None:
            self.first = frame
        self.last = frame
        count = len(smp) // 2
        self.samples += count
        prev = self.prev
        for i in range(count):
            off = cbo + i
            cur = (smp[2 * i], smp[2 * i + 1])
            old = prev.get(off)
            if old is not None:
                self.revisits += 1
            if old is not None and old == cur:
                if self.run is None or self.run[1] + self.run[2] != off:
                    self.end_run()
                    self.run = [frame, off, 0, 0]
                self.run[2] += 1
                pk = max(abs(cur[0]), abs(cur[1]))
                if pk > self.run[3]:
                    self.run[3] = pk
            else:
                self.end_run()
            prev[off] = cur


def analyse(path):
    voices = {}
    for frame, voice, cbo, smp in read_records(path):
        st = voices.get(voice)
        if st is None:
            st = voices[voice] = VoiceState()
        st.feed(frame, cbo, smp)
    for st in voices.values():
        st.end_run()
    return voices


def secs(frame):
    return frame / VP_FRAMES_PER_S


def summary(voices, out):
    out.write('voice  from_s    to_s  samples  revisit  repeated  runs\n')
    for v in sorted(voices):
        st = voices[v]
        pct = 100.0 * st.same / st.revisits if st.revisits else 0.0
        out.write('%5d %7.2f %7.2f %8d %8d %8d (%5.1f%%) %4d\n' % (
            v, secs(st.first), secs(st.last), st.samples, st.revisits,
            st.same, pct, len(st.runs)))


def voice_runs(voices, v, out):
    st = voices.get(v)
    if st is None:
        out.write('voice %d not in the dump\n' % v)
        return
    out.write('voice %d: %d repeated runs\n' % (v, len(st.runs)))
    for frame, off, length, peak in st.runs:
        out.write('  t=%8.3f s  cbo=%06X  len=%6d  peak=%6d\n' % (
            secs(frame), off, length, peak))


def write_wav(path, v, out_path, t0=None, t1=None, rate=48000):
    pcm = array.array('h')
    for frame, voice, cbo, smp in read_records(path):
        if voice != v:
            continue
        t = secs(frame)
        if (t0 is not None and t < t0) or (t1 is not None and t > t1):
            continue
        pcm.extend(smp)
    w = wave.open(out_path, 'wb')
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(rate)
    if sys.byteorder != 'little':
        pcm.byteswap()
    w.writeframes(pcm.tobytes())
    w.close()
    return len(pcm) // 2


RAW_RATE = 48000
MIN_COPY = 2 * MIN_RUN  # raw mode: shortest copy reported


def read_raw(path):
    """A toolkit v0.14 voice dump: array('h') L,R,... (a cut last frame dropped)."""
    with open(path, 'rb') as f:
        data = f.read()
    smp = array.array('h')
    smp.frombytes(data[:len(data) // 4 * 4])
    if sys.byteorder != 'little':
        smp.byteswap()
    return smp


def raw_stats(smp):
    """(frames, peak, rms, zero-crossing rate of L) of a raw dump."""
    n = len(smp) // 2
    if n == 0:
        return 0, 0, 0.0, 0.0
    peak = max(abs(x) for x in smp)
    sq = 0.0
    zc = 0
    last = 0
    for i in range(0, 2 * n, 2):
        x = smp[i]
        sq += x * x + smp[i + 1] * smp[i + 1]
        if (x < 0) != (last < 0):
            zc += 1
        last = x
    return n, peak, (sq / (2 * n)) ** 0.5, zc / n


def raw_copies(smp, min_len=MIN_COPY, min_level=MIN_LEVEL):
    """Stretches of >= min_len frames equal to earlier output, as
    (at_frame, from_frame, length, peak). Earlier windows of MIN_RUN frames
    are indexed at multiples of MIN_RUN and every later position is looked
    up, so any copy of 2 * MIN_RUN - 1 frames or more is found."""
    n = len(smp) // 2
    raw = smp.tobytes()
    w = MIN_RUN * 4                     # a window in bytes

    def win(k):
        return raw[4 * k:4 * k + w]

    index = {}
    runs = []
    p = 0
    while p + MIN_RUN <= n:
        if p % MIN_RUN == 0:
            index.setdefault(win(p), p)
        q = index.get(win(p))
        if q is None or q >= p:
            p += 1
            continue
        a, b = p, q                     # grow the match backwards ...
        while b > 0 and smp[2 * a - 2:2 * a] == smp[2 * b - 2:2 * b]:
            a -= 1
            b -= 1
        end = p + MIN_RUN               # ... and forwards
        while end < n and smp[2 * end:2 * end + 2] == smp[2 * (end - a + b):2 * (end - a + b) + 2]:
            end += 1
        if not runs or a >= runs[-1][0] + runs[-1][2]:
            peak = max(abs(x) for x in smp[2 * a:2 * end])
            if end - a >= min_len and peak >= min_level:
                runs.append((a, b, end - a, peak))
        for k in range(p + 1, end):     # keep indexing what is skipped
            if k % MIN_RUN == 0 and k + MIN_RUN <= n:
                index.setdefault(win(k), k)
        p = end
    return runs


def raw_summary(paths, out):
    out.write('file                         secs    peak     rms    zcr  copies  copied_s\n')
    for path in paths:
        smp = read_raw(path)
        n, peak, rms, zcr = raw_stats(smp)
        runs = raw_copies(smp)
        out.write('%-24s %8.2f %7d %7.0f %6.3f %7d %9.2f\n' % (
            os.path.basename(path)[-24:], n / RAW_RATE, peak, rms, zcr,
            len(runs), sum(r[2] for r in runs) / RAW_RATE))


def raw_runs(path, out):
    runs = raw_copies(read_raw(path))
    out.write('%s: %d copied stretches\n' % (path, len(runs)))
    for at, src, length, peak in runs:
        out.write('  t=%8.3f s  copy of t=%8.3f s  len=%6d  peak=%6d\n' % (
            at / RAW_RATE, src / RAW_RATE, length, peak))


def raw_wav(path, out_path, t0=None, t1=None, rate=RAW_RATE):
    smp = read_raw(path)
    n = len(smp) // 2
    i0 = max(0, int(t0 * RAW_RATE)) if t0 is not None else 0
    i1 = min(n, int(t1 * RAW_RATE)) if t1 is not None else n
    pcm = smp[2 * i0:2 * max(i0, i1)]
    w = wave.open(out_path, 'wb')
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(rate)
    if sys.byteorder != 'little':
        pcm.byteswap()
    w.writeframes(pcm.tobytes())
    w.close()
    return len(pcm) // 2


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump', nargs='+', help='the dump (raw mode: one or more files)')
    ap.add_argument('--raw', action='store_true',
                    help='toolkit v0.14 raw dump (the default for .raw files)')
    ap.add_argument('--voice', type=int, help='list that voice\'s repeated runs')
    ap.add_argument('--wav', nargs=2, metavar=('VOICE', 'OUT'),
                    help='write a voice\'s source samples as a WAV')
    ap.add_argument('--rate', type=int, default=48000,
                    help='WAV sample rate (the voice\'s source rate)')
    ap.add_argument('--from', dest='t0', type=float)
    ap.add_argument('--to', dest='t1', type=float)
    a = ap.parse_args(argv)
    if a.raw or all(d.lower().endswith('.raw') for d in a.dump):
        if a.wav:
            n = raw_wav(a.dump[0], a.wav[1], a.t0, a.t1, a.rate)
            print('wrote %d samples to %s' % (n, a.wav[1]))
        elif a.voice is not None:
            for d in a.dump:
                raw_runs(d, sys.stdout)
        else:
            raw_summary(a.dump, sys.stdout)
        return 0
    if len(a.dump) != 1:
        ap.error('the record format takes one dump file')
    a.dump = a.dump[0]
    if a.wav:
        n = write_wav(a.dump, int(a.wav[0]), a.wav[1], a.t0, a.t1, a.rate)
        print('wrote %d samples to %s' % (n, a.wav[1]))
        return 0
    voices = analyse(a.dump)
    if a.voice is not None:
        voice_runs(voices, a.voice, sys.stdout)
    else:
        summary(voices, sys.stdout)
    return 0


if __name__ == '__main__':
    sys.exit(main())
