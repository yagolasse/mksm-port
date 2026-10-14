"""padding_seeds.py on a synthetic code section (no game data)."""

import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import padding_seeds  # noqa: E402

VA, RAW = 0x00011000, 0x1000


class PaddingSeedsTest(unittest.TestCase):
    def run_tool(self, code, funcs, extra_sections=()):
        with tempfile.TemporaryDirectory() as tmp:
            xbe = Path(tmp, "game.xbe")
            xbe.write_bytes(bytes(RAW) + code)
            sections = [{"name": ".text", "virtual_addr": "0x%08X" % VA,
                         "raw_addr": "0x%X" % RAW, "raw_size": len(code), "executable": True}]
            sections += extra_sections
            Path(tmp, "game_analysis.json").write_text(json.dumps({"sections": sections}))
            fj = Path(tmp, "functions.json")
            fj.write_text(json.dumps(funcs))
            out = Path(tmp, "seeds.json")
            with redirect_stdout(StringIO()):
                self.assertEqual(padding_seeds.main(["x", str(xbe), str(fj), str(out)]), 0)
            return [s["start"] for s in json.loads(out.read_text())]

    def test_finds_uncovered_starts_after_int3(self):
        code = bytearray(b"\x90" * 0x80)
        code[0x0E:0x10] = b"\xcc\xcc"   # padding -> 0x10 starts a function
        code[0x2F:0x30] = b"\xcc"       # one int3 only: not padding
        code[0x4C:0x50] = b"\xcc" * 4   # padding -> 0x50, but 0x50 is int3 too
        code[0x50] = 0xCC
        code[0x6E:0x70] = b"\xcc\xcc"   # padding -> 0x70, covered by a function
        funcs = [{"start": "0x%08X" % (VA + 0x70), "end": "0x%08X" % (VA + 0x80)}]
        self.assertEqual(self.run_tool(bytes(code), funcs), ["0x%08X" % (VA + 0x10)])

    def test_int_addresses_and_data_sections(self):
        code = bytearray(b"\x90" * 0x40)
        code[0x1E:0x20] = b"\xcc\xcc"
        funcs = [{"start": VA + 0x20, "end": VA + 0x30}]  # ints, not hex strings
        data = {"name": ".rdata", "virtual_addr": "0x00020000", "raw_addr": "0x%X" % RAW,
                "raw_size": 0x40, "executable": True}  # same bytes, but a data section
        self.assertEqual(self.run_tool(bytes(code), funcs, [data]), [])


if __name__ == "__main__":
    unittest.main()
