#!/usr/bin/env python3
"""Sort the lifter's unresolved-target stubs into real code vs. noise.

tools.recomp writes every call/jump target it has no function for to
gen/recomp_stubs_unresolved.c as a stub that only pops the return address. A
`jmp`/`jcc` to one lifts as `sub_X(); return;`, so reaching a stub from real
code silently returns early from the function that jumped there. This tool
reads each stub's target, the lifted functions that reference it, and the
bytes around it, and puts it in one class:

  bogus      target is outside every code section's raw bytes (the jump was
             decoded from data)
  dead       every reference sits in unreachable lifted code, after a `goto`
             or `return` with no label in between (bytes after a switch's
             indirect jmp, usually its jump table, decoded as instructions)
  from-data  every lifted function that references it looks like data: a
             switch jump table taken for a function (MSVC parks them right
             after the function, often behind a `mov edi,edi` pair), or bytes
             that decode to hlt/in/out/les/... early on
  data       the target itself decodes to such instructions
  entry      a function start: follows int3 padding or a ret, or is the target
             of an incremental-link thunk `jmp`
  split      mid-function, reached from another detected function: the
             detector cut one real function into pieces and a piece jumps back

entry and split need a seed. A seed inside a function becomes an alias body
(target to the end of the enclosing function), which is what a tail jump into
the middle needs. --seeds writes those two classes in --seed-functions format.

Usage (from external/xboxrecomp):
    python ../../tools/stub_audit.py ../../../game_files/default.xbe \
        tools/disasm/output/functions.json ../../src/recomp/gen [--seeds ../../seeds/stubs.json]
"""

import argparse
import bisect
import glob
import json
import os
import re
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

DATA_SECTIONS = {".rdata", ".data", ".idata"}
THUNKS = (0x00011005, 0x00016CDA)  # incremental-link jmp table (see run-time lookup)
BAD = {"hlt", "in", "out", "insb", "insd", "insw", "outsb", "outsd", "outsw", "les", "lds",
       "arpl", "bound", "into", "iretd", "retf", "sti", "cli", "salc", "aas", "aaa", "das",
       "daa", "aam", "aad", "pushal", "popal", "int1"}

FN_RE = re.compile(r"^(?:static )?void (\w+)\(void\)\s*\{?\s*$")
LABEL_RE = re.compile(r"^\s*loc_([0-9A-F]{8}): ;")
END_RE = re.compile(r"^    (goto loc_[0-9A-F]{8};|return;|.*\(\); return; /\* tail jmp|RECOMP_ICALL_JMP|.*recomp_jmp)")
STUB_RE = re.compile(r"void (sub_[0-9A-F]{8})\(void\) \{ g_esp \+= \d+; /\* 0x([0-9A-F]{8}): ([^*]+?) \*/")


class Image:
    def __init__(self, xbe_path):
        self.data = Path(xbe_path).read_bytes()
        analysis = Path(xbe_path).with_name(Path(xbe_path).stem + "_analysis.json")
        self.sections = [s for s in json.loads(analysis.read_text())["sections"]
                         if s["executable"] and s["name"] not in DATA_SECTIONS]
        self.cs = Cs(CS_ARCH_X86, CS_MODE_32)

    def section(self, va):
        for s in self.sections:
            v = int(s["virtual_addr"], 16)
            if v <= va < v + s["raw_size"]:
                return s, int(s["raw_addr"], 16) + va - v
        return None, None

    def read(self, va, n):
        s, p = self.section(va)
        return self.data[p:p + n] if s else b""

    def insns(self, va, n=8, nbytes=64):
        return list(self.cs.disasm(self.read(va, nbytes), va))[:n]

    def looks_like_data(self, va, n=6):
        ins = self.insns(va, n)
        if len(ins) < n and sum(i.size for i in ins) < 16:
            return "undecodable"
        for i in ins:
            if i.mnemonic in BAD:
                return i.mnemonic
        return None

    def text_near(self, va):
        """A run of 8+ printable ASCII bytes within 16 bytes of va (a string)."""
        run = 0
        for b in self.read(va - 16, 32):
            run = run + 1 if 0x20 <= b < 0x7F else 0
            if run >= 8:
                return True
        return False

    def jump_table_at(self, va):
        """Two consecutive dwords pointing near va, starting within 8 bytes of it.

        Covers a table right at va (after an optional `mov edi,edi`) and a
        lifted label that landed mid-entry inside one.
        """
        raw = self.read(va - 8, 18)
        for k in range(len(raw) - 7):
            a, b = struct.unpack_from("<II", raw, k)
            if all(abs(t - va) < 0x10000 and self.section(t)[0] for t in (a, b)):
                return True
        return False


def lifted_refs(gen_dir, stubs):
    """stub name -> {lifted function name: [site VA of each reachable reference]}.

    The site is the nearest `loc_` label before the reference (else the
    function start), close enough to tell code from a decoded table. A
    reference after an unconditional `goto`/`return` with no label in between
    is dead code and gets no site: the lifter decoded the bytes after a
    switch's indirect jmp (its jump table) or past the function's end.
    """
    refs = {s: {} for s in stubs}
    for path in sorted(glob.glob(os.path.join(gen_dir, "recomp_0*.c"))):
        cur, site, dead = None, None, False
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FN_RE.match(line)
                if m:
                    cur, dead = m.group(1), False
                    site = int(cur[4:], 16) if re.fullmatch(r"sub_[0-9A-F]{8}", cur) else None
                    continue
                m = LABEL_RE.match(line)
                if m:
                    site, dead = int(m.group(1), 16), False
                for name in re.findall(r"sub_[0-9A-F]{8}", line):
                    if name in refs and name != cur:
                        sites = refs[name].setdefault(cur, [])
                        if not dead:
                            sites.append(site)
                if END_RE.match(line):
                    dead = True
    return refs


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("xbe")
    ap.add_argument("functions")
    ap.add_argument("gen_dir")
    ap.add_argument("--seeds", help="write entry/split targets as seeds to this file")
    args = ap.parse_args(argv[1:])

    img = Image(args.xbe)
    funcs = json.loads(Path(args.functions).read_text())
    by_name = {f["name"]: int(f["start"], 16) for f in funcs}
    bounds = sorted((int(f["start"], 16), int(f["end"], 16)) for f in funcs)
    starts = [b[0] for b in bounds]

    def containing(va):
        i = bisect.bisect_right(starts, va) - 1
        return bounds[i] if i >= 0 and bounds[i][0] <= va < bounds[i][1] else None

    stub_text = Path(args.gen_dir, "recomp_stubs_unresolved.c").read_text()
    stubs = {m.group(1): (int(m.group(2), 16), m.group(3)) for m in STUB_RE.finditer(stub_text)}
    refs = lifted_refs(args.gen_dir, stubs)

    rows, seeds = [], []
    for name, (va, note) in sorted(stubs.items(), key=lambda kv: kv[1][0]):
        live = sorted(s for s, sites in refs[name].items() if sites)
        sources = live + sorted("~" + s for s, sites in refs[name].items() if not sites)
        sec, _ = img.section(va)
        body = containing(va)
        why = ""
        if sec is None:
            cls = "bogus"
        elif not live:
            cls, why = "dead", "only after goto/return"
        else:
            # Classify every reachable reference site; from-data only if all are.
            site_data = []
            sites = [v for s in live for v in refs[name][s]]
            for v in sites:
                if v is None:
                    continue
                if img.jump_table_at(v):
                    site_data.append("jump table")
                elif img.text_near(v):
                    site_data.append("text")
                else:
                    bad = img.looks_like_data(v, 12)
                    if bad:
                        site_data.append(bad)
            src_vas = [by_name[s] for s in live if s in by_name]
            pre = img.read(va - 3, 3)
            bad = (img.looks_like_data(va, 4) or ("text" if img.text_near(va) else None)
                   or ("int3 padding" if img.read(va, 1) == b"\xcc" else None))
            if sites and len(site_data) == len(sites):
                cls, why = "from-data", ",".join(sorted(set(site_data)))
            elif bad:
                cls, why = "data", bad
            elif pre[-1:] in (b"\xcc", b"\xc3") or pre[:1] == b"\xc2":
                cls, why = "entry", "after int3" if pre[-1:] == b"\xcc" else "after ret"
            elif any(THUNKS[0] <= s <= THUNKS[1] for s in src_vas):
                cls, why = "entry", "thunk target"
            else:
                cls = "split"
                why = "in 0x%08X+0x%X" % (body[0], va - body[0]) if body else "in a gap"
        rows.append((va, cls, why, note, sources))
        if cls in ("entry", "split"):
            seeds.append({"start": "0x%08X" % va,
                          "note": "stub_audit: %s (%s), referenced from %s"
                                  % (cls, why, ", ".join(sources))})

    for va, cls, why, note, sources in rows:
        print("0x%08X  %-9s  %-22s  %-12s  %s" % (va, cls, why, note, " ".join(sources)))
    counts = {}
    for r in rows:
        counts[r[1]] = counts.get(r[1], 0) + 1
    print("\n%d stubs: %s" % (len(rows), ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
    if args.seeds:
        Path(args.seeds).write_text(json.dumps(seeds, indent=2) + "\n")
        print("%d seeds -> %s" % (len(seeds), args.seeds))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
