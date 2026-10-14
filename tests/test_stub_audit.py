"""stub_audit.py's parsing of lifted C (synthetic gen dir, no game data)."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
try:
    import stub_audit  # needs capstone
except ImportError:  # pragma: no cover
    stub_audit = None

GEN = """\
void sub_00011000(void) {
    sub_00099999();
    goto loc_00011010;
    sub_00088888();
    loc_00011010: ;
    sub_00077777(); return; /* tail jmp */
    sub_00066666();
}

void sub_00012000(void) {
    sub_00099999();
    sub_00011000();
}
"""

STUBS = """\
void sub_00099999(void) { g_esp += 4; /* 0x00099999: not detected */ }
void sub_00088888(void) { g_esp += 4; /* 0x00088888: ret 8 */ }
"""


@unittest.skipIf(stub_audit is None, "capstone not installed")
class StubAuditTest(unittest.TestCase):
    def test_stub_regex(self):
        found = {m.group(1): (m.group(2), m.group(3)) for m in stub_audit.STUB_RE.finditer(STUBS)}
        self.assertEqual(found, {"sub_00099999": ("00099999", "not detected"),
                                 "sub_00088888": ("00088888", "ret 8")})

    def test_lifted_refs_sites_and_dead_code(self):
        with tempfile.TemporaryDirectory() as gen:
            Path(gen, "recomp_0000.c").write_text(GEN)
            Path(gen, "recomp_stubs_unresolved.c").write_text(STUBS)  # not scanned
            refs = stub_audit.lifted_refs(gen, ["sub_00099999", "sub_00088888",
                                                "sub_00077777", "sub_00066666",
                                                "sub_00011000"])
        # Site = nearest label before the reference, else the function start.
        self.assertEqual(refs["sub_00099999"], {"sub_00011000": [0x00011000],
                                                "sub_00012000": [0x00012000]})
        # After a goto with no label in between: dead, so no site.
        self.assertEqual(refs["sub_00088888"], {"sub_00011000": []})
        self.assertEqual(refs["sub_00077777"], {"sub_00011000": [0x00011010]})
        # After a tail jmp ... return: dead.
        self.assertEqual(refs["sub_00066666"], {"sub_00011000": []})
        # A function's own name in its header is not a reference.
        self.assertEqual(refs["sub_00011000"], {"sub_00012000": [0x00012000]})


if __name__ == "__main__":
    unittest.main()
