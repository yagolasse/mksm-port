"""smoke_test.py's log checks on synthetic logs (does not run the exe)."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import smoke_test  # noqa: E402

BOOT = next(c for c in smoke_test.CHECKPOINTS if c.name == "boot")
PAD = next(c for c in smoke_test.CHECKPOINTS if c.name == "pad")

GOOD_BOOT = """\
  [PATH] \\Device\\CdRom0\\FRONT\\MOVIES\\MIDWAY.SFD
[GPU] draws 1200 (1200 with coordinates), 10 indices; x 0..1  y 0..1
  [PATH] \\Device\\CdRom0\\FRONT\\MOVIES\\OPENING.SFD
[GPU] draws %d (%d with coordinates), 10 indices; x 0..1  y 0..1
""" % (BOOT.min_draws, BOOT.min_draws)


def failures(cp, code, text):
    return [m for ok, m in smoke_test.check(cp, code, text) if not ok]


class SmokeChecksTest(unittest.TestCase):
    def test_good_boot_passes(self):
        self.assertEqual(failures(BOOT, 3, GOOD_BOOT), [])
        self.assertEqual(failures(BOOT, 0, GOOD_BOOT), [])

    def test_exit_code(self):
        self.assertEqual(len(failures(BOOT, 1, GOOD_BOOT)), 1)
        self.assertEqual(len(failures(BOOT, None, GOOD_BOOT)), 1)  # timed out

    def test_crash_and_unresolved(self):
        text = GOOD_BOOT + "[CRASH] Access violation at RIP=0x1\n" \
            + "[ICALL] Failed to resolve VA 0x0045AD10\n"
        self.assertEqual(len(failures(BOOT, 3, text)), 2)

    def test_wild_icall_allowlist(self):
        known = "[ICALL] target 0x00000140 is not code -- skipped 1 time(s)\n"
        self.assertEqual(failures(BOOT, 3, GOOD_BOOT + known), [])
        other = "[ICALL] target 0x00000150 is not code -- skipped 1 time(s)\n"
        self.assertEqual(len(failures(BOOT, 3, GOOD_BOOT + other)), 1)

    def test_draws_take_the_max_and_files_are_required(self):
        low = GOOD_BOOT.replace("draws %d (" % BOOT.min_draws, "draws 5 (")
        self.assertEqual(len(failures(BOOT, 3, low)), 1)
        no_opening = GOOD_BOOT.replace("OPENING.SFD", "OTHER.SFD")
        self.assertEqual(failures(BOOT, 3, no_opening), ["opened OPENING.SFD"])

    def test_pad_steps(self):
        text = GOOD_BOOT + "".join(
            "  [PATH] \\Device\\X\\%s\n" % f for f in PAD.files) \
            + "[GPU] draws %d (" % PAD.min_draws + "\n"
        steps = "".join("  PAD: t=%d ms step %d (digital 0x10 analog 0x00)\n" % (i * 1000, i)
                        for i in range(PAD.last_pad_step + 1))
        self.assertEqual(failures(PAD, 3, text + steps), [])
        self.assertEqual(len(failures(PAD, 3, text)), 1)

    def test_pad_script_matches_1_12c(self):
        entries = smoke_test.PAD_SCRIPT.split(",")
        self.assertEqual(len(entries), 12)
        self.assertEqual([e.split(":")[1] for e in entries], ["start"] * 5 + ["a"] * 7)


if __name__ == "__main__":
    unittest.main()
