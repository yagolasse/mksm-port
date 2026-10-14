"""wad.py on small synthetic archives (no game data)."""

import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import wad  # noqa: E402


def packed(offset, stored, unpacked):
    """Encode an entry in the packed (bit 31) form wad.Wad._decode reads."""
    assert offset % 0x800 == 0 and unpacked % 8 == 0
    a = (offset >> 11) | ((stored & 0x3FF) << 22)
    b = 0x80000000 | (stored >> 10) | ((unpacked >> 3) << 12)
    return a, b


def ewdf(payload):
    """EWDF blob: magic, two u32, then the DEFLATE offset at +8, then raw DEFLATE."""
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    stream = co.compress(payload) + co.flush()
    return b"EWDF" + struct.pack("<III", len(payload), 0x10, 0) + stream


def build(entries_data, version=2, table_at=None):
    """Archive bytes: PWF header, table, then each blob at a 0x800-aligned offset.

    Returns (archive, [(offset, blob)]).
    """
    count = len(entries_data)
    table = table_at if table_at is not None else (0x800 if version == 1 else 0x20)
    pos = (table + count * 8 + 0x7FF) & ~0x7FF
    placed = []
    for blob in entries_data:
        placed.append((pos, blob))
        pos = (pos + len(blob) + 0x7FF) & ~0x7FF
    out = bytearray(pos)
    out[0:0x20] = b"PWF " + struct.pack("<7I", 0, version, count, 0, 0, 0, 0)
    for i, (off, blob) in enumerate(placed):
        if blob[:4] == b"EWDF":
            unpacked = (struct.unpack_from("<I", blob, 4)[0] + 7) & ~7
            a, b = packed(off, len(blob), unpacked)
        else:
            a, b = off, len(blob)
        struct.pack_into("<II", out, table + i * 8, a, b)
        out[off:off + len(blob)] = blob
    return bytes(out), placed


class WadTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        if getattr(self, "w", None):
            for f in self.w.files:
                f.close()
        self.tmp.cleanup()

    def write(self, name, data):
        Path(self.dir, name).write_bytes(data)

    def test_decode_plain_and_packed(self):
        self.assertEqual(wad.Wad._decode(0x1234, 0x56), (0x1234, 0x56, 0x56))
        a, b = packed(0x12345800, 0x2C0FF, 0x7FFF8)
        self.assertEqual(wad.Wad._decode(a, b), (0x12345800, 0x2C0FF, 0x7FFF8))

    def test_plain_and_ewdf_entries(self):
        text = b"hello shaolin " * 50
        data, placed = build([b"XPR0" + b"\x01" * 60, ewdf(text), b""])
        self.write("GAMEDATA.WAD", data)
        self.w = wad.Wad(self.dir)
        self.assertEqual(self.w.table_offset, 0x20)
        self.assertEqual(len(self.w.entries), 3)
        self.assertEqual(self.w.entries[0], (placed[0][0], 64, 64))
        self.assertEqual(self.w.data(0), placed[0][1])
        self.assertEqual(self.w.raw(1)[:4], b"EWDF")
        self.assertEqual(self.w.data(1), text)
        self.assertEqual(self.w.entries[2][1], 0)

    def test_version_1_table_at_0x800(self):
        data, _ = build([b"ABCD"], version=1)
        self.write("GAMEDATA.WAD", data)
        self.w = wad.Wad(self.dir)
        self.assertEqual(self.w.table_offset, 0x800)
        self.assertEqual(self.w.data(0), b"ABCD")

    def test_zero_first_entry_falls_back_to_0x800(self):
        # Version 2, but the table actually sits at 0x800 (+0x20 is zeros).
        data, _ = build([b"WXYZ"], version=2, table_at=0x800)
        self.write("GAMEDATA.WAD", data)
        self.w = wad.Wad(self.dir)
        self.assertEqual(self.w.table_offset, 0x800)
        self.assertEqual(self.w.data(0), b"WXYZ")

    def test_split_windows(self):
        # Shrink the window so the archive spans .WAD/.WAE/.WAF without 300 MiB files.
        # Like the real archive, no entry crosses a window: wad.py reads each
        # entry from one file (checked on the retail data, 2026-10-14).
        data, placed = build([b"A" * 0x700, b"B" * 0x10, b"C" * 0x800, b"D" * 0x10])
        window = 0x1000
        old = wad.WINDOW
        wad.WINDOW = window
        try:
            for i in range(0, len(data), window):
                self.write(wad.SPLIT_NAMES[i // window], data[i:i + window])
            self.w = wad.Wad(self.dir)
            self.assertFalse(self.w.single)
            self.assertGreaterEqual(len(self.w.files), 3)
            for i, (_, blob) in enumerate(placed):
                self.assertEqual(self.w.data(i), blob)
        finally:
            wad.WINDOW = old

    def test_bad_magic_and_missing(self):
        self.write("GAMEDATA.WAD", b"NOPE" + bytes(0x40))
        with self.assertRaises(ValueError):
            wad.Wad(self.dir)
        os.remove(Path(self.dir, "GAMEDATA.WAD"))
        with self.assertRaises(FileNotFoundError):
            wad.Wad(self.dir)

    def test_guess_ext(self):
        self.assertEqual(wad.guess_ext(b"AFS\0rest"), ".afs")
        self.assertEqual(wad.guess_ext(b"XPR0...."), ".xpr0")
        self.assertEqual(wad.guess_ext(b"PWF ...."), ".pwf")
        self.assertEqual(wad.guess_ext(b"\x00\x01\x02\x03"), ".bin")


if __name__ == "__main__":
    unittest.main()
