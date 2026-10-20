"""Regression tests for the Cocogitto source version hook."""

import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "ci/set-version.py"
CMAKE = "CMakeLists.txt"
DRIVER = "src/btrfs.inf"
VOLUME = "src/btrfs-vol.inf"


class VersionHookTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="winbtrfs-version-")
        self.addCleanup(self.scratch.cleanup)
        self.root = pathlib.Path(self.scratch.name)
        (self.root / "ci").mkdir()
        (self.root / "src").mkdir()
        shutil.copyfile(SCRIPT, self.root / "ci/set-version.py")
        self.write(CMAKE, b"project(btrfs VERSION 1.10.0)\r\n")
        self.write(DRIVER, b"DriverVer   = 09/01/2026,1.10.0.0\r\n")
        self.write(VOLUME, b"DriverVer   = 09/01/2026,1.10.0.0\n")

    def write(self, name, content):
        (self.root / name).write_bytes(content)

    def read(self, name):
        return (self.root / name).read_bytes()

    def snapshot(self):
        return {name: self.read(name) for name in (CMAKE, DRIVER, VOLUME)}

    def run_hook(self, version):
        return subprocess.run(
            [sys.executable, str(self.root / "ci/set-version.py"), version],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_updates_versions_and_preserves_line_endings(self):
        result = self.run_hook("1.11.0")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.read(CMAKE), b"project(btrfs VERSION 1.11.0)\r\n")
        self.assertRegex(self.read(DRIVER), rb"^DriverVer   = \d{2}/\d{2}/\d{4},1\.11\.0\.0\r\n$")
        self.assertRegex(self.read(VOLUME), rb"^DriverVer   = \d{2}/\d{2}/\d{4},1\.11\.0\.0\n$")

    def test_rejects_versions_that_do_not_advance(self):
        before = self.snapshot()
        for version in ("1.8.3", "1.10.0", "1.11.0-beta.1"):
            with self.subTest(version=version):
                result = self.run_hook(version)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.snapshot(), before)

    def test_rejects_inconsistent_source_versions_without_writes(self):
        self.write(VOLUME, b"DriverVer   = 09/01/2026,1.9.0.0\n")
        before = self.snapshot()

        result = self.run_hook("1.11.0")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("versions differ", result.stderr)
        self.assertEqual(self.snapshot(), before)


if __name__ == "__main__":
    unittest.main()
