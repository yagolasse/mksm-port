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

Stdlib only.
"""

import argparse
import array
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump')
    ap.add_argument('--voice', type=int, help='list that voice\'s repeated runs')
    ap.add_argument('--wav', nargs=2, metavar=('VOICE', 'OUT'),
                    help='write a voice\'s source samples as a WAV')
    ap.add_argument('--rate', type=int, default=48000,
                    help='WAV sample rate (the voice\'s source rate)')
    ap.add_argument('--from', dest='t0', type=float)
    ap.add_argument('--to', dest='t1', type=float)
    a = ap.parse_args(argv)
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
