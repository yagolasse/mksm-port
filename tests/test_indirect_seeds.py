"""indirect_seeds.py on a synthetic image (no game data)."""

import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import indirect_seeds  # noqa: E402

TEXT_VA, TEXT_RAW, TEXT_SIZE = 0x00011000, 0x1000, 0x1000
DATA_VA, DATA_RAW, DATA_SIZE = 0x00030000, 0x2000, 0x100


def sections():
    return [
        {"name": ".text", "virtual_addr": "0x%08X" % TEXT_VA, "raw_addr": "0x%08X" % TEXT_RAW,
         "raw_size": TEXT_SIZE, "executable": True},
        # the analysis marks data sections executable too; the name decides
        {"name": ".data", "virtual_addr": "0x%08X" % DATA_VA, "raw_addr": "0x%08X" % DATA_RAW,
         "raw_size": DATA_SIZE, "executable": True},
    ]


def build():
    xbe = bytearray(DATA_RAW + DATA_SIZE)

    def jmp(at, to):
        o = TEXT_RAW + at - TEXT_VA
        xbe[o:o + 5] = b"\xe9" + struct.pack("<i", to - (at + 5))

    jmp(0x00011005, 0x00011800)          # thunk -> code
    jmp(0x0001100A, 0x00011900)
    jmp(0x0001100F, 0x00500000)          # thunk -> outside the image: not code
    xbe[TEXT_RAW + 0x14] = 0xCC          # padding inside the range
    jmp(0x00011015, 0x00011800)          # second thunk to the same target
    jmp(0x0001101A, DATA_VA + 8)         # thunk -> a data section: not code
    return bytes(xbe)


class IndirectSeedsTest(unittest.TestCase):
    def test_thunk_targets(self):
        secs = indirect_seeds.load_sections(sections())
        got = indirect_seeds.ilt_targets(build(), secs, 0x00011005, 0x0001101F)
        self.assertEqual(got, {0x00011800: 0x00011005, 0x00011900: 0x0001100A})

    def test_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            xbe = os.path.join(tmp, "default.xbe")
            Path(xbe).write_bytes(build())
            Path(tmp, "default_analysis.json").write_text(json.dumps({"sections": sections()}))
            out = os.path.join(tmp, "indirect.json")
            self.assertEqual(indirect_seeds.main([xbe, out, "--ilt", "0x00011005-0x0001101F"]), 0)
            rows = json.loads(Path(out).read_text())
            self.assertEqual([r["start"] for r in rows], ["0x00011800", "0x00011900"])
            self.assertIn("0x00011005", rows[0]["note"])


if __name__ == "__main__":
    unittest.main()
