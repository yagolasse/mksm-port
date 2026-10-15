"""smoke_test.py's log checks on synthetic logs (does not run the exe)."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import smoke_test  # noqa: E402

BOOT = next(c for c in smoke_test.CHECKPOINTS if c.name == "boot")
PAD = next(c for c in smoke_test.CHECKPOINTS if c.name == "pad")

GOOD_BOOT = """\
[APU] DSP doorbell 0x831C8810: command 0x00000003 acknowledged
  [PATH] \\Device\\CdRom0\\FRONT\\MOVIES\\MIDWAY.SFD
[GPU] draws 1200 (1200 with coordinates), 10 indices; x 0..1  y 0..1
  [PATH] \\Device\\CdRom0\\FRONT\\MOVIES\\OPENING.SFD
[GPU] draws %d (%d with coordinates), 10 indices; x 0..1  y 0..1
""" % (BOOT.min_draws, BOOT.min_draws)


def failures(cp, code, text):
    return [m for ok, m in smoke_test.check(cp, code, text) if not ok]


def failures_cap(cp, code, text):
    return [m for ok, m in
            smoke_test.check(cp, code, text, capture_after="WADCRC.BIN") if not ok]


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

    def test_contig_exhausted_and_dsp_ack(self):
        oom = "  [CONTIG] arena exhausted (2457600 requested, 65470464 of 67108864 used)\n"
        self.assertEqual(failures(BOOT, 3, GOOD_BOOT + oom), ["1 [CONTIG] arena exhausted"])
        no_ack = GOOD_BOOT.replace("acknowledged", "pending")
        self.assertEqual(failures(BOOT, 3, no_ack), ["DSP doorbell acknowledged"])

    def test_pad_steps(self):
        text = GOOD_BOOT + "".join(
            "  [PATH] \\Device\\X\\%s\n" % f for f in PAD.files) \
            + "[GPU] draws %d (" % PAD.min_draws + "\n"
        steps = "".join("  PAD: t=%d ms step %d (digital 0x10 analog 0x00)\n" % (i * 1000, i)
                        for i in range(PAD.last_pad_step + 1))
        step, names = PAD.files_after_step
        after = "".join("  [PATH] \\Device\\X\\%s\n" % f for f in names)
        self.assertEqual(failures(PAD, 3, text + steps + after), [])
        # no steps: the step check and each after-step file check fail
        self.assertEqual(len(failures(PAD, 3, text + after)), 1 + len(names))

    def test_pad_files_after_step(self):
        step, names = PAD.files_after_step
        files = "".join("  [PATH] \\Device\\X\\%s\n" % f for f in PAD.files + names)
        text = GOOD_BOOT.splitlines(True)[0] + files + "[GPU] draws %d (\n" % PAD.min_draws
        steps = "".join("  PAD: t=%d ms step %d (digital 0x10 analog 0x00)\n" % (i * 1000, i)
                        for i in range(PAD.last_pad_step + 1))
        # opened only before the step: fails once per file
        self.assertEqual(failures(PAD, 3, text + steps),
                         ["opened %s after pad step %d" % (n, step) for n in names])

    def test_pad_script_matches_1_12c(self):
        entries = smoke_test.PAD_SCRIPT.split(",")
        self.assertEqual(len(entries), 12)
        self.assertEqual([e.split(":")[1] for e in entries], ["start"] * 5 + ["a"] * 7)

    def test_capture_wrote_after_wadcrc(self):
        wadcrc = "  [PATH] \\Device\\CdRom0\\WADCRC.BIN\n"
        wrote = "[PBCAP] wrote C:\\logs\\pbcap_level_65000.pbcap (flip 2100, 123 frame methods)\n"
        # wrote after the level-load file opens -> the level-frame capture check passes
        self.assertEqual(failures_cap(BOOT, 3, GOOD_BOOT + wadcrc + wrote), [])
        # wrote before it (still in the title/movie) -> fails
        self.assertEqual(failures_cap(BOOT, 3, GOOD_BOOT + wrote + wadcrc),
                         ["[PBCAP] wrote after WADCRC.BIN"])
        # no WADCRC at all (capture never fired or never reached the level) -> fails
        self.assertEqual(failures_cap(BOOT, 3, GOOD_BOOT + wrote),
                         ["[PBCAP] wrote after WADCRC.BIN"])



if __name__ == "__main__":
    unittest.main()
