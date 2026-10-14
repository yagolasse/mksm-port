#!/usr/bin/env python3
"""Seed functions that start right after int3 padding but were never detected.

MSVC pads between functions with 0xCC up to a 16-byte boundary. A function that
nothing `call`s (a vtable method, a callback, a decoder picked from a table)
can sit after such padding and still be missed by tools.disasm: it is not in
any detected function and has no seed. At runtime it shows up as
"[ICALL] Failed to resolve VA ..." and usually a crash.

This scans every executable code section for 16-byte-aligned addresses that
follow at least two 0xCC bytes, are not 0xCC themselves, and lie outside every
function in functions.json, then writes them in the --seed-functions format.

Usage (from external/xboxrecomp):
    python ../../tools/padding_seeds.py ../../../game_files/default.xbe \
        tools/disasm/output/functions.json ../../seeds/padding.json

Run it against a functions.json produced *without* this seed file, or it finds
nothing new. Check the output: data placed after padding would decode too.
"""

import bisect
import json
import sys
from pathlib import Path

DATA_SECTIONS = {".rdata", ".data", ".idata"}


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    xbe = Path(argv[1]).read_bytes()
    analysis = Path(argv[1]).with_name(Path(argv[1]).stem + "_analysis.json")
    sections = json.loads(analysis.read_text())["sections"]
    funcs = json.loads(Path(argv[2]).read_text())

    as_int = lambda v: int(v, 16) if isinstance(v, str) else v
    bounds = sorted((as_int(f["start"]), as_int(f["end"])) for f in funcs)
    starts = [b[0] for b in bounds]

    def covered(va):
        i = bisect.bisect_right(starts, va) - 1
        return i >= 0 and bounds[i][0] <= va < bounds[i][1]

    seeds = []
    for s in sections:
        if not s["executable"] or s["name"] in DATA_SECTIONS:
            continue
        va0, raw, size = int(s["virtual_addr"], 16), int(s["raw_addr"], 16), s["raw_size"]
        for off in range(16 - va0 % 16, size, 16):
            p = raw + off
            if xbe[p] == 0xCC or xbe[p - 1] != 0xCC or xbe[p - 2] != 0xCC:
                continue
            if covered(va0 + off):
                continue
            seeds.append({"start": "0x%08X" % (va0 + off),
                          "note": "after int3 padding in %s, outside any detected function"
                                  % s["name"]})

    Path(argv[3]).write_text(json.dumps(seeds, indent=2) + "\n")
    print("%d seeds -> %s" % (len(seeds), argv[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
