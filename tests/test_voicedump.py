"""voicedump.py on synthetic dumps (no game data)."""

import array
import os
import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import voicedump  # noqa: E402


def record(frame, voice, cbo, mono):
    smp = array.array("h")
    for v in mono:
        smp.extend((v, v))
    if sys.byteorder == "big":
        smp.byteswap()
    return struct.pack("<IHHI", frame, voice, len(mono), cbo) + smp.tobytes()


RING = 1024
BATCH = 32


def ring_dump(stale_lap=None, stale_at=0, stale_len=0, voice=50):
    """A voice that loops over a RING-sample buffer the 'game' refills with
    new data each lap; in stale_lap the refill misses [stale_at, +stale_len)."""
    out = b""
    frame = 0
    buf = [0] * RING
    for lap in range(4):
        for i in range(RING):
            if not (lap == stale_lap and stale_at <= i < stale_at + stale_len):
                buf[i] = ((i * 37 + lap * 4999) % 20000) - 10000
        for off in range(0, RING, BATCH):
            out += record(frame, voice, off, buf[off:off + BATCH])
            frame += 1
    return out


class VoiceDumpTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.tmp.name, "d.bin")

    def tearDown(self):
        self.tmp.cleanup()

    def analyse(self, data):
        with open(self.path, "wb") as f:
            f.write(data)
        return voicedump.analyse(self.path)

    def test_refilled_ring_has_no_repeats(self):
        v = self.analyse(ring_dump())[50]
        self.assertEqual(v.samples, 4 * RING)
        self.assertEqual(v.revisits, 3 * RING)
        self.assertEqual(v.same, 0)
        self.assertEqual(v.runs, [])

    def test_late_refill_is_found(self):
        v = self.analyse(ring_dump(stale_lap=2, stale_at=200, stale_len=300))[50]
        self.assertEqual(len(v.runs), 1)
        frame, off, length, peak = v.runs[0]
        self.assertEqual(off, 200)
        self.assertEqual(length, 300)
        self.assertGreater(peak, voicedump.MIN_LEVEL)
        self.assertEqual(frame, (2 * RING + 192) // BATCH)  # the batch holding 200

    def test_short_and_silent_repeats_are_ignored(self):
        # stale for fewer than MIN_RUN samples
        v = self.analyse(ring_dump(stale_lap=1, stale_at=10, stale_len=20))[50]
        self.assertEqual(v.runs, [])
        # a silent loop repeats by design
        data = b"".join(record(f, 64, 0, [0] * 32) for f in range(10))
        v = self.analyse(data)[64]
        self.assertEqual(v.revisits, 9 * 32)
        self.assertEqual(v.runs, [])

    def test_voices_are_separate_and_truncation_is_tolerated(self):
        data = ring_dump(voice=1) + ring_dump(voice=2)
        data += struct.pack("<IHHI", 0, 3, 100, 0) + b"\x00" * 10  # cut short
        voices = self.analyse(data)
        self.assertEqual(sorted(voices), [1, 2])

    def test_wav_export(self):
        with open(self.path, "wb") as f:
            f.write(ring_dump())
        out = os.path.join(self.tmp.name, "v.wav")
        n = voicedump.write_wav(self.path, 50, out, rate=24000)
        self.assertEqual(n, 4 * RING)
        with wave.open(out) as w:
            self.assertEqual(w.getframerate(), 24000)
            self.assertEqual(w.getnframes(), 4 * RING)


def raw_bytes(mono):
    smp = array.array("h")
    for v in mono:
        smp.extend((v, v))
    if sys.byteorder == "big":
        smp.byteswap()
    return smp.tobytes()


def music(n, seed=0):
    """Non-repeating, non-silent test signal."""
    return [((i * 7919 + seed) * 104729 % 40000) - 20000 for i in range(n)]


class RawDumpTest(unittest.TestCase):
    """The toolkit v0.14 format: raw s16 stereo, one voice per file."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, data):
        path = os.path.join(self.tmp.name, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def test_fresh_signal_has_no_copies(self):
        smp = voicedump.read_raw(self.write("a.raw", raw_bytes(music(20000))))
        self.assertEqual(voicedump.raw_copies(smp), [])

    def test_stale_stretch_is_found_at_any_offset(self):
        sig = music(20000)
        sig[12345:12345 + 500] = sig[3001:3001 + 500]   # unaligned both ends
        smp = voicedump.read_raw(self.write("b.raw", raw_bytes(sig)))
        runs = voicedump.raw_copies(smp)
        self.assertEqual(len(runs), 1)
        at, src, length, peak = runs[0]
        self.assertEqual((at, src, length), (12345, 3001, 500))
        self.assertGreater(peak, voicedump.MIN_LEVEL)

    def test_short_and_silent_copies_are_ignored(self):
        sig = music(20000)
        sig[9000:9000 + voicedump.MIN_RUN] = sig[100:100 + voicedump.MIN_RUN]
        sig[15000:16000] = [0] * 1000
        sig[17000:18000] = [0] * 1000
        smp = voicedump.read_raw(self.write("c.raw", raw_bytes(sig)))
        self.assertEqual(voicedump.raw_copies(smp), [])

    def test_stats_cut_frame_and_wav(self):
        data = raw_bytes([1000, -1000] * 2400) + b"\x01\x02"  # half a frame left over
        path = self.write("d.raw", data)
        smp = voicedump.read_raw(path)
        n, peak, rms, zcr = voicedump.raw_stats(smp)
        self.assertEqual((n, peak), (4800, 1000))
        self.assertAlmostEqual(rms, 1000.0)
        self.assertAlmostEqual(zcr, 1.0, places=3)
        out = os.path.join(self.tmp.name, "d.wav")
        self.assertEqual(voicedump.raw_wav(path, out, t0=0.025, t1=0.075), 2400)
        with wave.open(out) as w:
            self.assertEqual(w.getframerate(), 48000)

    def test_cli_picks_raw_mode_from_the_extension(self):
        import io
        from contextlib import redirect_stdout
        p1 = self.write("v.1.raw", raw_bytes(music(5000)))
        p2 = self.write("v.2.raw", raw_bytes(music(5000, seed=3)))
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertEqual(voicedump.main([p1, p2]), 0)
        lines = buf.getvalue().splitlines()
        self.assertEqual(len(lines), 3)
        self.assertIn("v.1.raw", lines[1])


if __name__ == "__main__":
    unittest.main()
