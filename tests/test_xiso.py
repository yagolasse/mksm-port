"""xiso.py on a tiny synthetic XDVDFS image (no game data)."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import xiso  # noqa: E402

S = xiso.SECTOR


def dirent(name, start, size, attr=0x20, left=0, right=0):
    """One directory entry, padded to 4 bytes like XDVDFS."""
    raw = struct.pack("<HHIIBB", left, right, start, size, attr, len(name)) + name.encode()
    return raw + b"\xff" * (-len(raw) % 4)


def tree(entries):
    """A directory table from [(name, start, size, attr)]: each entry links the
    next one as its right child, the last one is followed by 0xFFFF padding."""
    blobs = [dirent(*e) for e in entries]
    out, offs, pos = [], [], 0
    for b in blobs:
        offs.append(pos)
        pos += len(b)
    for i, b in enumerate(blobs):
        if i + 1 < len(blobs):
            b = b[:2] + struct.pack("<H", offs[i + 1] // 4) + b[4:]
        out.append(b)
    table = b"".join(out)
    return table + b"\xff" * (S - len(table))


def image(a_name="A.TXT", sub_sector=34):
    """Root (sector 33): A.TXT, EMPTY (empty dir), SUB (dir at 34 holding
    B.BIN). File data at sectors 35 and 36 (B.BIN is longer than a sector)."""
    a, b = b"alpha\n", bytes(range(256)) * 10
    sub = tree([("B.BIN", 36, len(b), 0x20)])
    root = tree([(a_name, 35, len(a), 0x20), ("EMPTY", 0, 0, 0x10),
                 ("SUB", sub_sector, S, 0x10)])
    img = bytearray(38 * S)
    img[32 * S:32 * S + 28] = xiso.MAGIC + struct.pack("<II", 33, S)
    img[33 * S:34 * S] = root
    img[34 * S:35 * S] = sub
    img[35 * S:35 * S + len(a)] = a
    img[36 * S:36 * S + len(b)] = b
    return bytes(img), a, b


class XIsoTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)  # cleanups run last, after open() closes its files
        data, self.a, self.b = image()
        self.path = Path(self.tmp.name, "test.iso")
        self.path.write_bytes(data)
        self.iso = xiso.XIso(self.path)

    def tearDown(self):
        self.iso.f.close()

    def test_walk(self):
        self.assertEqual(self.iso.base, 0)
        entries = {p: (start, size, d) for p, start, size, d in self.iso.walk()}
        self.assertEqual(entries, {
            "A.TXT": (35, len(self.a), False),
            "EMPTY": (0, 0, True),
            "SUB": (34, S, True),
            "SUB/B.BIN": (36, len(self.b), False),
        })

    def test_extract(self):
        out = Path(self.tmp.name, "out", "SUB", "B.BIN")
        self.iso.extract(36, len(self.b), str(out))
        self.assertEqual(out.read_bytes(), self.b)

    def open(self, data):
        path = Path(self.tmp.name, f"other{len(list(Path(self.tmp.name).iterdir()))}.iso")
        path.write_bytes(data)
        iso = xiso.XIso(path)
        self.addCleanup(iso.f.close)
        return iso

    def test_truncated_image(self):
        """Extracting a file whose data is cut off raises instead of looping forever."""
        data, _, b = image()
        iso = self.open(data[:36 * S + 100])
        with self.assertRaises(ValueError):
            iso.extract(36, len(b), str(Path(self.tmp.name, "out", "B.BIN")))

    def test_dir_loop(self):
        """A directory whose table is its parent's stops at the depth limit."""
        iso = self.open(image(sub_sector=33)[0])
        with self.assertRaises(ValueError):
            list(iso.walk())

    def test_bad_names(self):
        for name in ("..", "A\\B", "C:", "X.", "\x01"):
            with self.subTest(name=name):
                iso = self.open(image(a_name=name)[0])
                with self.assertRaises(ValueError):
                    list(iso.walk())

    def test_no_volume_descriptor(self):
        bad = Path(self.tmp.name, "bad.iso")
        bad.write_bytes(bytes(40 * S))
        with self.assertRaises(ValueError):
            xiso.XIso(bad)


if __name__ == "__main__":
    unittest.main()
