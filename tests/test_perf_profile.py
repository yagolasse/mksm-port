"""perf_profile.py's parsers on synthetic /proc and `perf script` text (no game data, no perf)."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import perf_profile  # noqa: E402

# Two samples of thread 101, one of thread 202 (leaf frame first, as perf script prints them).
SCRIPT = """\
mksm_recomp   101
\t    55d0000010 sub_00406C12+0x1a (/x/mksm_recomp)
\t    55d0000020 sub_002B4940+0x30 (/x/mksm_recomp)
\t    55d0000030 thread_trampoline+0x12 (/x/mksm_recomp)
\t    7f00000040 start_thread+0x2f (/usr/lib64/libc.so.6)

mksm_recomp   101
\t    55d0000020 sub_002B4940+0x30 (/x/mksm_recomp)
\t    55d0000030 thread_trampoline+0x12 (/x/mksm_recomp)
\t    7f00000040 start_thread+0x2f (/usr/lib64/libc.so.6)

radeonsi:cs0   202
\t    7f00000050 [unknown] ([unknown])

"""


# `perf report --sort pid,sym -t ';'` (percent of all samples).
REPORT = """\
# Samples: 10
     9.00%  ;  101:mksm_recomp   ;[.] sub_00406C12
     3.00%  ;  101:mksm_recomp   ;[.] pthread_mutex_lock@@GLIBC_2.2.5
     1.50%  ;  202:radeonsi:cs0  ;[.] _mesa_unpack_depth_span
"""


class PerfProfileTest(unittest.TestCase):
    def test_task_stat(self):
        # comm with a space and parentheses; utime 7, stime 5 (fields 14/15).
        line = "42 (a (b) c) S 1 2 3 4 5 6 7 8 9 10 7 5 0 0 20 0 3 0 100"
        self.assertEqual(perf_profile.parse_task_stat(line), ("a (b) c", 12))

    def test_cpu_shares(self):
        before = {1: ("a", 100), 2: ("b", 0)}
        after = {1: ("a", 150), 2: ("b", 100), 3: ("new", 10)}
        rows = perf_profile.cpu_shares(before, after, secs=1.0, hz=100)
        self.assertEqual(rows, [(2, "b", 100.0), (1, "a", 50.0), (3, "new", 10.0)])

    def test_parse_perf_script(self):
        t = perf_profile.parse_perf_script(SCRIPT)
        self.assertEqual(set(t), {101, 202})
        a = t[101]
        self.assertEqual(a["samples"], 2)
        self.assertEqual(a["self"], {"sub_00406C12": 1, "sub_002B4940": 1})
        self.assertEqual(a["incl"]["sub_002B4940"], 2)
        # Glue frames dropped, outermost useful frame first.
        self.assertEqual(a["root"].most_common(1)[0], ("sub_002B4940 < sub_00406C12", 1))
        self.assertEqual(t[202]["self"], {"[unknown]": 1})

    def test_parse_perf_report(self):
        f = perf_profile.parse_perf_report(REPORT)
        self.assertEqual(f[101], {"sub_00406C12": 9.0, "pthread_mutex_lock": 3.0})
        self.assertEqual(f[202], {"_mesa_unpack_depth_span": 1.5})

    def test_rank_keys(self):
        t = perf_profile.parse_perf_script(SCRIPT)
        shares = [(7, "gl0", 9.0), (101, "mksm_recomp", 8.0), (8, "gl0", 1.0)]
        self.assertEqual(perf_profile.rank_keys(shares, t), [
            ("gl0 #1", "", 9.0), ("mksm_recomp #1", "sub_002B4940 < sub_00406C12", 8.0),
            ("gl0 #2", "", 1.0)])

    def test_short(self):
        self.assertEqual(perf_profile.short("amdgpu_cs_add_buffer(radeon_cmdbuf*, int)"),
                         "amdgpu_cs_add_buffer")
        self.assertEqual(perf_profile.short("x" * 60, 10), "xxxxxxx...")

    def test_summarize(self):
        t = perf_profile.parse_perf_script(SCRIPT)
        flat = perf_profile.parse_perf_report(REPORT)
        shares = [(101, "mksm_recomp", 80.0), (202, "radeonsi:cs0", 1.0)]
        out = perf_profile.summarize(shares, t, flat, 15.0)
        self.assertEqual(out[0], "window 15 s; total 0.81 cores")
        self.assertIn("thread 101 (mksm_recomp): CPU 80%", out)
        self.assertIn("  self: sub_00406C12 75.0, pthread_mutex_lock 25.0", out)
        self.assertIn("  root: sub_002B4940 < sub_00406C12 (50% of 2 unwound)", out)
        self.assertTrue(out[-1].startswith("1 threads below 2%"))


if __name__ == "__main__":
    unittest.main()
