#!/usr/bin/env python3
"""Seed the filter and handler blocks of MSVC SEH scope tables.

A function with __try/__except or __try/__finally registers a scope table:
an array of {previous level, filter, handler} records in a data section. The
filter and handler blocks are code that only the CRT's exception handler
calls (when an exception is dispatched, or when RtlUnwind runs __finally
blocks on the way past), through a pointer read from that table. Nothing
calls or jumps to them, so tools.disasm may not make entries of them: they
stay inside their function's body or in a gap. Unwinding into one then fails
at runtime with "[ICALL] Failed to resolve VA ...".

This finds the tables two ways, then writes every filter and handler they
name in the --seed-functions format (a seed inside a detected function
becomes an alias entry, so it does not split that function):
  - push <table>; call __SEH_prolog, where __SEH_prolog is recognised by its
    first bytes (push handler; mov eax, fs:[0]; push eax; mov eax, [esp+10h];
    mov [esp+10h], ebp; lea ebp, [esp+10h]);
  - the inline frame: push -1; push <table>; push <handler>; mov eax, fs:[0].
Records are read from the first one while the previous level is -1 or names
an earlier record and the pointers are in code.

Usage:
    python tools/seh_seeds.py ../game_files/default.xbe seeds/seh.json

Needs default_analysis.json (tools.xbe_parser) next to the XBE. Writes only
addresses. Stdlib only.
"""

import json
import struct
import sys
from pathlib import Path

DATA_SECTIONS = {".rdata", ".data", ".idata"}
# __SEH_prolog after its leading push of the frame handler (5 bytes).
SEH_PROLOG_TAIL = bytes.fromhex("64a10000000050" "8b442410" "896c2410" "8d6c2410")
INLINE_FS = bytes.fromhex("64a100000000")
MAX_RECORDS = 64


class Image:
    def __init__(self, xbe, sections):
        self.xbe = xbe
        self.secs = [(s["name"], int(s["virtual_addr"], 16), int(s["raw_addr"], 16),
                      s["raw_size"]) for s in sections]
        self.code = [s for s in self.secs if s[0] not in DATA_SECTIONS
                     and next(x for x in sections if x["name"] == s[0])["executable"]]
        self.data = [s for s in self.secs if s[0] in DATA_SECTIONS]

    @staticmethod
    def _find(secs, va):
        for s in secs:
            if s[1] <= va < s[1] + s[3]:
                return s
        return None

    def is_code(self, va):
        return self._find(self.code, va) is not None

    def is_data(self, va):
        return self._find(self.data, va) is not None

    def offset(self, va):
        s = self._find(self.secs, va)
        return None if s is None else s[2] + va - s[1]


def find_seh_prologs(img):
    out = set()
    for name, va, raw, size in img.code:
        seg = img.xbe[raw:raw + size]
        i = seg.find(SEH_PROLOG_TAIL)
        while i != -1:
            if i >= 5 and seg[i - 5] == 0x68:
                out.add(va + i - 5)
            i = seg.find(SEH_PROLOG_TAIL, i + 1)
    return out


def find_tables(img, prologs):
    tables = set()
    for name, va, raw, size in img.code:
        seg = img.xbe[raw:raw + size]
        i = seg.find(b"\x68")
        while i != -1 and i + 10 <= len(seg):
            t = struct.unpack_from("<I", seg, i + 1)[0]
            if img.is_data(t):
                if seg[i + 5] == 0xE8:
                    dest = va + i + 10 + struct.unpack_from("<i", seg, i + 6)[0]
                    if dest in prologs:
                        tables.add(t)
                if (i >= 2 and seg[i - 2:i] == b"\x6a\xff" and seg[i + 5] == 0x68
                        and seg[i + 10:i + 16] == INLINE_FS):
                    tables.add(t)
            i = seg.find(b"\x68", i + 1)
    return tables


def table_targets(img, table):
    """{target: table} for the filter/handler pointers of one scope table."""
    out = {}
    o = img.offset(table)
    for k in range(MAX_RECORDS):
        if o is None or o + 12 * k + 12 > len(img.xbe):
            break
        prev, filt, handler = struct.unpack_from("<III", img.xbe, o + 12 * k)
        if not (prev == 0xFFFFFFFF or prev < k):
            break
        if not img.is_code(handler) or not (filt == 0 or img.is_code(filt)):
            break
        for a in (filt, handler):
            if a:
                out.setdefault(a, table)
    return out


def scan(xbe, sections):
    img = Image(xbe, sections)
    prologs = find_seh_prologs(img)
    tables = find_tables(img, prologs)
    targets = {}
    for t in sorted(tables):
        for a, tab in table_targets(img, t).items():
            targets.setdefault(a, tab)
    return prologs, tables, targets


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    xbe_path = Path(argv[1])
    xbe = xbe_path.read_bytes()
    analysis = xbe_path.with_name(xbe_path.stem + "_analysis.json")
    sections = json.loads(analysis.read_text())["sections"]
    prologs, tables, targets = scan(xbe, sections)
    seeds = [{"start": "0x%08X" % a,
              "note": "SEH scope-table filter/handler (table 0x%08X)" % targets[a]}
             for a in sorted(targets)]
    Path(argv[2]).write_text(json.dumps(seeds, indent=2) + "\n", newline="\n")
    print("%d __SEH_prolog, %d scope tables, %d filter/handler seeds -> %s"
          % (len(prologs), len(tables), len(seeds), argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
