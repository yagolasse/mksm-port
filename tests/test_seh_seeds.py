"""seh_seeds.py on a synthetic image (no game data)."""

import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import seh_seeds  # noqa: E402

TEXT_VA, TEXT_RAW, TEXT_SIZE = 0x00011000, 0x1000, 0x1000
RDATA_VA, RDATA_RAW, RDATA_SIZE = 0x00020000, 0x2000, 0x100
PROLOG = TEXT_VA + 0x800


def sections():
    return [
        {"name": ".text", "virtual_addr": "0x%08X" % TEXT_VA, "raw_addr": "0x%08X" % TEXT_RAW,
         "raw_size": TEXT_SIZE, "executable": True},
        # the analysis marks data sections executable too; the name decides
        {"name": ".rdata", "virtual_addr": "0x%08X" % RDATA_VA, "raw_addr": "0x%08X" % RDATA_RAW,
         "raw_size": RDATA_SIZE, "executable": True},
    ]


def image(records_a, records_b):
    xbe = bytearray(RDATA_RAW + RDATA_SIZE)

    def put(va, data):
        if TEXT_VA <= va < TEXT_VA + TEXT_SIZE:
            o = TEXT_RAW + va - TEXT_VA
        else:
            o = RDATA_RAW + va - RDATA_VA
        xbe[o:o + len(data)] = data

    # __SEH_prolog: push handler; mov eax, fs:[0]; ...
    put(PROLOG, b"\x68" + struct.pack("<I", TEXT_VA + 0x900) + seh_seeds.SEH_PROLOG_TAIL)
    # f1: push 10h; push table_a; call __SEH_prolog
    call_at = TEXT_VA + 0x102
    put(TEXT_VA + 0x100, b"\x6a\x10\x68" + struct.pack("<I", RDATA_VA)
        + b"\xe8" + struct.pack("<i", PROLOG - (call_at + 10)))
    # f2: push -1; push table_b; push handler; mov eax, fs:[0]
    put(TEXT_VA + 0x200, b"\x6a\xff\x68" + struct.pack("<I", RDATA_VA + 0x40)
        + b"\x68" + struct.pack("<I", TEXT_VA + 0x900) + bytes.fromhex("64a100000000"))
    # a push of a data address that is no scope table
    put(TEXT_VA + 0x300, b"\x68" + struct.pack("<I", RDATA_VA + 0x80) + b"\xe8\x00\x00\x00\x00")
    for i, rec in enumerate(records_a):
        put(RDATA_VA + 12 * i, struct.pack("<III", *rec))
    for i, rec in enumerate(records_b):
        put(RDATA_VA + 0x40 + 12 * i, struct.pack("<III", *rec))
    put(RDATA_VA + 0x80, struct.pack("<III", 0xFFFFFFFF, 0, TEXT_VA + 0x777))
    return bytes(xbe)


class SehSeedsTest(unittest.TestCase):
    def test_both_frame_kinds_and_chained_records(self):
        a = [(0xFFFFFFFF, 0, TEXT_VA + 0x150),               # __finally: no filter
             (0, TEXT_VA + 0x160, TEXT_VA + 0x170),          # nested __except
             (5, TEXT_VA + 0x180, TEXT_VA + 0x190)]          # bad level: the table ends
        b = [(0xFFFFFFFF, TEXT_VA + 0x250, TEXT_VA + 0x260),
             (0xFFFFFFFF, 0, RDATA_VA + 0x10)]               # handler not in code: ends
        prologs, tables, targets = seh_seeds.scan(image(a, b), sections())
        self.assertEqual(prologs, {PROLOG})
        self.assertEqual(tables, {RDATA_VA, RDATA_VA + 0x40})
        self.assertEqual(sorted(targets), [TEXT_VA + 0x150, TEXT_VA + 0x160, TEXT_VA + 0x170,
                                           TEXT_VA + 0x250, TEXT_VA + 0x260])
        self.assertEqual(targets[TEXT_VA + 0x160], RDATA_VA)
        self.assertNotIn(TEXT_VA + 0x777, targets)          # not pushed by a frame

    def test_writes_seed_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            xbe = os.path.join(tmp, "default.xbe")
            with open(xbe, "wb") as f:
                f.write(image([(0xFFFFFFFF, 0, TEXT_VA + 0x150)], []))
            with open(os.path.join(tmp, "default_analysis.json"), "w") as f:
                json.dump({"sections": sections()}, f)
            out = os.path.join(tmp, "seh.json")
            self.assertEqual(seh_seeds.main(["seh_seeds.py", xbe, out]), 0)
            seeds = json.load(open(out))
            self.assertEqual([s["start"] for s in seeds], ["0x%08X" % (TEXT_VA + 0x150)])
            self.assertIn("scope-table", seeds[0]["note"])


if __name__ == "__main__":
    unittest.main()
