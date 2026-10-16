#!/usr/bin/env python3
"""Seed the targets of the incremental-link thunks.

An incremental-link (ILT) table is a run of `jmp rel32` thunks; code calls
the thunk and the thunk jumps to the function. The runtime follows a thunk
(recomp_lookup_manual), but its target must be a function or alias, or the
call fails with "[ICALL] Failed to resolve VA". A function reached only
through its thunk has no direct caller, so the disassembler can miss it
(toolkit v0.14's did for 27, e.g. 0x00036740 and 0x0006AC30 behind the thunks
at 0x00012842 and 0x0001508D). Every target of a thunk in --ilt LO-HI that
lies in a code section is written; a thunk target is a function start by
construction, so a seed there can't cut a function.

Not done here, on purpose: carrying over entries an older lift had and a
data table points to. Small data values look like pointers (0x00020004...),
the older lift had junk aliases at such addresses, and seeding them cut a
function into one-instruction pieces whose loop then recursed until the host
stack overflowed (plan H.5b1). Such entries go through the crash loop
instead (tools.seed_from_log -> seeds/observed.json).

Usage (from port/):
    python tools/indirect_seeds.py ../game_files/default.xbe seeds/indirect.json \\
        --ilt 0x00011005-0x00016CDA

Needs default_analysis.json (tools.xbe_parser) next to the XBE. Writes only
addresses. Stdlib only.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

DATA_SECTIONS = {".rdata", ".data", ".idata"}


def load_sections(sections):
    out = []
    for s in sections:
        out.append((s["name"], int(s["virtual_addr"], 16), int(s["raw_addr"], 16),
                    s["raw_size"], s["executable"] and s["name"] not in DATA_SECTIONS))
    return out


def in_code(secs, va):
    return any(code and v <= va < v + n for _, v, _r, n, code in secs)


def to_raw(secs, va):
    for _, v, r, n, _c in secs:
        if v <= va < v + n:
            return r + va - v
    return None


def ilt_targets(xbe, secs, lo, hi):
    """{target: first thunk} for every jmp rel32 thunk in [lo, hi]."""
    out = {}
    va = lo
    while va <= hi:
        o = to_raw(secs, va)
        if o is not None and o + 5 <= len(xbe) and xbe[o] == 0xE9:
            t = va + 5 + struct.unpack_from("<i", xbe, o + 1)[0]
            if in_code(secs, t):
                out.setdefault(t, va)
            va += 5
        else:
            va += 1
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("xbe")
    ap.add_argument("out")
    ap.add_argument("--ilt", required=True, metavar="LO-HI",
                    help="the incremental-link thunk range, hex")
    a = ap.parse_args(argv)
    xbe_path = Path(a.xbe)
    xbe = xbe_path.read_bytes()
    analysis = xbe_path.with_name(xbe_path.stem + "_analysis.json")
    secs = load_sections(json.loads(analysis.read_text())["sections"])
    lo, hi = (int(x, 16) for x in a.ilt.split("-"))
    rows = [{"start": "0x%08X" % t,
             "note": "target of the incremental-link thunk at 0x%08X" % thunk}
            for t, thunk in sorted(ilt_targets(xbe, secs, lo, hi).items())]
    Path(a.out).write_text(json.dumps(rows, indent=2) + "\n", newline="\n")
    print("%d thunk-target seeds -> %s" % (len(rows), a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
