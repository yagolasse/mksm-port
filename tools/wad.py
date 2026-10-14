#!/usr/bin/env python3
"""Reader for the GAMEDATA.WA? archive of MK: Shaolin Monks (Xbox).

Usage:
    wad.py info    <game_dir>
    wad.py list    <game_dir>
    wad.py extract <game_dir> <out_dir> [first_index [count]]

<game_dir> is the folder holding GAMEDATA.WAD, GAMEDATA.WAE, ...

Layout:
  - The GAMEDATA.WA? files are one logical archive cut into 300 MiB windows:
    .WAD holds bytes [0, 300M), .WAE [300M, 600M) and so on. If GAMEDATA.WAD is
    itself bigger than one window, it is the whole archive.
  - It starts with a 0x20 byte "PWF " header (0x800 when the version field is 1),
    followed by `count` 8-byte entries.
  - Entry, two u32 (a, b). When b has bit 31 set it is packed:
        offset   = (a & 0x1FFFFF) << 11        (2 KiB sectors)
        stored   = (a >> 22) | ((b & 0xFFF) << 10)
        unpacked = ((b >> 12) & 0x7FFFF) << 3
    otherwise offset = a and stored = unpacked = b.
  - Stored data that starts with "EWDF" is compressed; the u32 at +8 is the
    offset of a raw DEFLATE stream.
"""
import os
import struct
import sys
import zlib

WINDOW = 0x12C00000
SPLIT_NAMES = [f"GAMEDATA.WA{c}" for c in "DEFGHIJKLMN"]


class Wad:
    def __init__(self, game_dir):
        self.files = []
        for name in SPLIT_NAMES:
            path = os.path.join(game_dir, name)
            if not os.path.exists(path):
                break
            self.files.append(open(path, "rb"))
        if not self.files:
            raise FileNotFoundError(f"no GAMEDATA.WAD in {game_dir}")
        self.single = os.fstat(self.files[0].fileno()).st_size > WINDOW

        head = self._read(0, 0x20)
        if head[:4] != b"PWF ":
            for f in self.files:
                f.close()
            raise ValueError(f"bad magic {head[:4]!r}")
        self.header = struct.unpack("<8I", head)
        version, count = self.header[2], self.header[3]
        table = 0x800 if version == 1 else 0x20
        first = self._read(table, 8)
        if first == b"\0" * 8:
            table = 0x800
        self.table_offset = table
        raw = self._read(table, count * 8)
        self.entries = [self._decode(*struct.unpack_from("<II", raw, i * 8)) for i in range(count)]

    @staticmethod
    def _decode(a, b):
        if b & 0x80000000:
            return ((a & 0x1FFFFF) << 11,
                    (a >> 22) | ((b & 0xFFF) << 10),
                    ((b >> 12) & 0x7FFFF) << 3)
        return a, b, b

    def _read(self, offset, size):
        if self.single:
            f, local = self.files[0], offset
        else:
            f, local = self.files[offset // WINDOW], offset % WINDOW
        f.seek(local)
        return f.read(size)

    def raw(self, index):
        offset, stored, _ = self.entries[index]
        return self._read(offset, stored)

    def data(self, index):
        """Entry contents, decompressed if EWDF."""
        blob = self.raw(index)
        if blob[:4] != b"EWDF":
            return blob
        start = struct.unpack_from("<I", blob, 8)[0]
        return zlib.decompressobj(-15).decompress(blob[start:])


def guess_ext(blob):
    magic = blob[:4]
    if magic == b"AFS\0":
        return ".afs"
    if magic.isalnum() or magic.rstrip(b" \0").isalnum():
        return "." + magic.rstrip(b" \0").decode("ascii").lower()
    return ".bin"


def main(argv):
    if len(argv) < 3 or argv[1] not in ("info", "list", "extract"):
        print(__doc__)
        return 1
    wad = Wad(argv[2])
    if argv[1] == "info":
        print("header:", " ".join(f"{v:08X}" for v in wad.header))
        print(f"entries: {len(wad.entries)}  table at 0x{wad.table_offset:X}  "
              f"split files: {len(wad.files)}  single: {wad.single}")
        return 0
    if argv[1] == "list":
        for i, (off, stored, unpacked) in enumerate(wad.entries):
            magic = wad.raw(i)[:4] if stored else b""
            print(f"{i:5} id={i + 0x14:5}  off=0x{off:09X}  stored={stored:9}  "
                  f"unpacked={unpacked:9}  {magic!r}")
        return 0
    out_dir = argv[3]
    first = int(argv[4]) if len(argv) > 4 else 0
    count = int(argv[5]) if len(argv) > 5 else len(wad.entries) - first
    os.makedirs(out_dir, exist_ok=True)
    for i in range(first, min(first + count, len(wad.entries))):
        if not wad.entries[i][1]:
            continue
        blob = wad.data(i)
        name = f"{i:05}{guess_ext(blob)}"
        with open(os.path.join(out_dir, name), "wb") as out:
            out.write(blob)
        print(name, len(blob))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
