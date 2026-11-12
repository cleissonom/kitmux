from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/package-release.sh"


class ReleasePackagingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="km-package-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.binary = self.root / "kitmux"
        self.binary.write_text("#!/bin/sh\nprintf 'kitmux 0.5.0\\n'\n")
        self.binary.chmod(0o755)
        self.licenses = self.root / "licenses"
        self.licenses.mkdir()
        for name in ("json-c", "libyaml", "openssl", "ncurses", "tomlc99"):
            (self.licenses / (name + ".txt")).write_text(name + " license\n")
        self.output = self.root / "dist"

    def package(self, tag="v0.5.0", platform="linux", arch="arm64"):
        return subprocess.run(
            ["sh", str(SCRIPT), str(self.binary), tag, platform, arch,
             str(self.licenses), str(self.output)],
            cwd=ROOT, capture_output=True, text=True, timeout=10)

    def test_archive_contains_versioned_executable_and_notices(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        stem = "kitmux-v0.5.0-linux-arm64"
        with tarfile.open(self.output / (stem + ".tar.gz")) as archive:
            binary = archive.getmember(stem + "/kitmux")
            self.assertTrue(binary.mode & 0o111)
            self.assertIn(stem + "/LICENSE", archive.getnames())
            self.assertIn(stem + "/THIRD_PARTY.md", archive.getnames())
            for name in ("json-c", "libyaml", "openssl", "ncurses", "tomlc99"):
                self.assertIn(stem + "/licenses/" + name + ".txt", archive.getnames())

    def test_tag_mismatch_does_not_create_an_archive(self):
        result = self.package(tag="v0.6.0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("version", result.stderr.lower())
        self.assertFalse(self.output.exists())

    def test_invalid_target_and_missing_license_fail_before_packaging(self):
        for platform, arch in (("windows", "arm64"), ("linux", "../escape")):
            result = self.package(platform=platform, arch=arch)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(self.output.exists())
        (self.licenses / "openssl.txt").unlink()
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("license", result.stderr.lower())
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
