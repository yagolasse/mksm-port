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


if __name__ == "__main__":
    unittest.main()
