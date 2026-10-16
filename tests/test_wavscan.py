"""wavscan.py on synthetic signals (no game data)."""

import array
import io
import math
import os
import random
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import wavscan  # noqa: E402

RATE = 48000


def sine(n, freq=440.0, amp=8000, phase=0.0):
    return [int(amp * math.sin(phase + 2 * math.pi * freq * i / RATE)) for i in range(n)]


def write(path, mono):
    """Stereo WAV with the same samples in both channels."""
    a = array.array("h")
    for v in mono:
        a.extend((v, v))
    if sys.byteorder == "big":
        a.byteswap()
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(a.tobytes())


class WavScanTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def scan(self, mono):
        p = os.path.join(self.tmp.name, "t.wav")
        write(p, mono)
        return wavscan.scan(p)

    def kinds(self, flags):
        out = set()
        for f in flags:
            out |= f
        return out

    def test_clean_sine(self):
        *_, flags, evs = self.scan(sine(RATE))
        self.assertEqual(evs, [])

    def test_noise_burst(self):
        rnd = random.Random(1)
        s = sine(RATE)
        for i in range(24000, 24000 + 2560):          # 10 blocks of garbage
            s[i] = rnd.randint(-12000, 12000)
        rate, ch, n, blocks, flags, evs = self.scan(s)
        noisy = [i for i, f in enumerate(flags) if "noisy" in f]
        self.assertTrue(set(range(94, 103)) <= set(noisy), noisy)
        self.assertTrue(any(abs(e[0] - 0.5) < 0.01 for e in evs))

    def test_click(self):
        s = sine(RATE, amp=3000, freq=200)
        s[10000] = 30000
        *_, flags, evs = self.scan(s)
        self.assertIn("click", flags[10000 // 256])
        self.assertEqual(len(evs), 1)

    def test_repeat_and_dropout(self):
        s = sine(RATE, freq=300)
        s[256 * 20:256 * 21] = s[256 * 19:256 * 20]   # block 20 = block 19
        s[256 * 40:256 * 41] = [0] * 256
        *_, flags, _ = self.scan(s)
        self.assertIn("repeat", flags[20])
        self.assertIn("dropout", flags[40])

    def test_silence_is_clean(self):
        *_, flags, evs = self.scan([0] * RATE)
        self.assertEqual(evs, [])

    def test_clip(self):
        s = sine(RATE, amp=40000)
        s = [max(-32768, min(32767, v)) for v in s]
        *_, flags, _ = self.scan(s)
        self.assertIn("clip", self.kinds(flags))

    def test_spacing_and_report(self):
        s = sine(RATE * 2, amp=3000, freq=200)
        for k in range(10):                           # a click every 20 blocks
            s[256 * (10 + 20 * k) + 100] = 30000
        p = os.path.join(self.tmp.name, "t.wav")
        write(p, s)
        buf = io.StringIO()
        evs = wavscan.report(p, 5, True, out=buf)
        self.assertEqual(len(evs), 10)
        self.assertIn("20: 9", buf.getvalue())

    def test_excerpt(self):
        p = os.path.join(self.tmp.name, "t.wav")
        q = os.path.join(self.tmp.name, "c.wav")
        write(p, sine(RATE))
        wavscan.excerpt(p, 0.25, 0.5, q)
        with wave.open(q, "rb") as w:
            self.assertEqual(w.getnframes(), RATE // 2)


if __name__ == "__main__":
    unittest.main()
