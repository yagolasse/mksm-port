"""Runs the settings file's C unit test (tests/settings_test.c, plan B7b1) if it is built.

The test itself is C (src/settings.c is C); CMake builds it as settings_test.exe and CTest
runs it as `settings`. This wrapper makes `python -m unittest discover -s tests` run it too,
and skips when the exe isn't built yet.
"""
import subprocess
import unittest
from pathlib import Path

BUILD = Path(__file__).resolve().parent.parent / "build"


def find_exe():
    for cfg in ("Release", "RelWithDebInfo", "Debug"):
        exe = BUILD / cfg / "settings_test.exe"
        if exe.is_file():
            return exe
    return None


class SettingsC(unittest.TestCase):
    def test_settings_c(self):
        exe = find_exe()
        if exe is None:
            self.skipTest("settings_test.exe not built (cmake --build build --target settings_test)")
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("settings_test: OK", r.stdout)


if __name__ == "__main__":
    unittest.main()
