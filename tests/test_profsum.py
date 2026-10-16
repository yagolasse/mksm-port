"""profsum.py on a synthetic run log (no game data)."""

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import profsum  # noqa: E402


def xgpu(draws, flips):
    return (f"[XGPU] methods 16384, objects 5, draws {draws}, flips {flips}, nop notifies 1, "
            "fences 1, presents 1 (no surface 0, last 640x480, 1 non-black px)")


LOG = "\n".join([
    "[PROF] armed: start 62000 ms, 15000 ms, period 1000 us",
    xgpu(0, 0),
    xgpu(300, 300),             # menu: 1 draw/flip, 60 fps
    xgpu(150300, 600),          # level: 500 draws/flip, 60 fps
    "[PROF] sampling 23 threads for 15000 ms",
    xgpu(250300, 800),          # straddles the window start: not "in"
    xgpu(325300, 950),          # in the window, 30 fps
    "[PROF] 15000 samples over 15016 ms (period 1000 us, 23 threads seen)",
    "[PROF] thread 11 (worker A): 100 samples, CPU 9000 ms = 60% of the window",
    "    exclusive:",
    "       50.0%     50  mksm_recomp!sub_00001000",
    "       25.0%     25  ntdll!ZwYieldExecution",
    "  [NV2A] DMA_PUT = 0x0  DMA_GET = 0x0",
    "    inclusive:",
    "      100.0%    100  mksm_recomp!sub_00002000",
    "[PROF] thread 22 (render): 100 samples, CPU 3000 ms = 20% of the window",
    "    exclusive:",
    "       90.0%     90  ntdll!ZwWaitForSingleObject",
    "    inclusive:",
    "      100.0%    100  ntdll!RtlUserThreadStart",
])


class ProfsumTest(unittest.TestCase):
    def test_parse(self):
        r = profsum.parse(LOG, top=5)
        self.assertEqual(r["window_ms"], 15016)
        self.assertEqual([(f, d) for f, d, _ in r["intervals"]],
                         [(60.0, 1), (60.0, 500), (40.0, 500), (30.0, 500)])
        self.assertEqual([i for _, _, i in r["intervals"]], [False, False, False, True])
        self.assertEqual(profsum.level_fps(r["intervals"], 300), [60.0, 40.0, 30.0])
        names = [(n, p, ms) for n, p, ms, _, _ in r["threads"]]
        self.assertEqual(names, [("worker A", 60, 9000), ("render", 20, 3000)])
        self.assertEqual(r["threads"][0][3],
                         [("mksm_recomp!sub_00001000", 50.0), ("ntdll!ZwYieldExecution", 25.0)])
        self.assertEqual(r["threads"][1][3], [("ntdll!ZwWaitForSingleObject", 90.0)])
        self.assertEqual(r["threads"][0][4], [("mksm_recomp!sub_00002000", 100.0)])
        self.assertEqual(r["threads"][1][4], [("ntdll!RtlUserThreadStart", 100.0)])

    def test_main_median(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d, "a.log"), Path(d, "b.log")
            a.write_text(LOG)
            b.write_text(LOG.replace("= 60% of", "= 80% of"))
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(profsum.main([str(a), str(b), "--top", "1"]), 0)
        text = out.getvalue()
        self.assertIn("total: 80% = 0.80 cores", text)
        self.assertIn("70.0%  (60/80)  worker A", text)
        self.assertIn("total: median 0.90 cores (0.80/1.00)", text)


if __name__ == "__main__":
    unittest.main()
