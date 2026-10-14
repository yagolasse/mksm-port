#!/usr/bin/env python3
"""Minimal XDVDFS (Xbox ISO) reader: list and extract files.

Usage:
    xiso.py list    <image.iso>
    xiso.py extract <image.iso> <out_dir> [path_filter]
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Redump images carry a video partition first; XISO images start directly.
PARTITION_OFFSETS = (0, 0x18300000, 0x0FD90000, 0x02080000)


class XIso:
    def __init__(self, path):
        self.f = open(path, "rb")
        for base in PARTITION_OFFSETS:
            self.f.seek(base + 32 * SECTOR)
            if self.f.read(20) == MAGIC:
                self.base = base
                break
        else:
            raise ValueError("no XDVDFS volume descriptor found")
        self.root_sector, self.root_size = struct.unpack("<II", self.f.read(8))

    def _read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def walk(self, sector=None, size=None, prefix=""):
        """Yield (path, start_sector, size, is_dir) for every entry."""
        if sector is None:
            sector, size = self.root_sector, self.root_size
        if size == 0:
            return
        data = self._read(sector, size)
        seen = set()
        stack = [0]
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
            if left == 0xFFFF:  # sector padding
                continue
            name = data[off + 14 : off + 14 + nlen].decode("ascii", "replace")
            path = f"{prefix}{name}"
            is_dir = bool(attr & 0x10)
            yield path, start, fsize, is_dir
            if is_dir:
                yield from self.walk(start, fsize, path + "/")
            if left:
                stack.append(left * 4)
            if right:
                stack.append(right * 4)

    def extract(self, start, size, out_path):
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        self.f.seek(self.base + start * SECTOR)
        with open(out_path, "wb") as out:
            remaining = size
            while remaining:
                chunk = self.f.read(min(remaining, 1 << 20))
                out.write(chunk)
                remaining -= len(chunk)


def main(argv):
    if len(argv) < 3 or argv[1] not in ("list", "extract"):
        print(__doc__)
        return 1
    iso = XIso(argv[2])
    if argv[1] == "list":
        total = 0
        for path, start, size, is_dir in sorted(iso.walk()):
            if not is_dir:
                total += size
                print(f"{size:>12}  {path}")
        print(f"{total:>12}  total (partition offset 0x{iso.base:X})")
        return 0
    out_dir = argv[3]
    filt = argv[4].lower() if len(argv) > 4 else ""
    for path, start, size, is_dir in iso.walk():
        if not is_dir and filt in path.lower():
            iso.extract(start, size, os.path.join(out_dir, *path.split("/")))
            print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
