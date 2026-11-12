import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ReleaseVerificationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="km-verify-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.binary = self.root / "kitmux"
        self.binary.write_text("#!/bin/sh\nprintf 'kitmux 0.5.0\\n'\n")
        self.binary.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.root) + ":" + os.environ["PATH"])
        self.tool("file", "printf 'ELF 64-bit aarch64\\n'")
        self.tool("xcrun", "printf 'minos 15.0\\n'")
        self.readelf("libc.so.6", "2.35")

    def tool(self, name, script):
        target = self.root / name
        target.write_text("#!/bin/sh\n" + script + "\n")
        target.chmod(0o755)

    def readelf(self, library, glibc):
        self.tool("readelf", "if [ \"$1\" = -d ]; then\n"
                  f"printf '0 (NEEDED) Shared library: [{library}]\\n'\n"
                  f"else printf 'Name: GLIBC_{glibc}\\n'; fi")

    def verify(self, platform="linux"):
        return subprocess.run(["sh", str(ROOT / "scripts/verify-release.sh"),
                               str(self.binary), platform, "arm64"], env=self.env,
                              text=True, capture_output=True, timeout=10)

    def test_linux_accepts_baseline_and_rejects_build_libraries_and_new_glibc(self):
        self.assertEqual(self.verify().returncode, 0)
        self.readelf("libcrypto.so.3", "2.35")
        self.assertNotEqual(self.verify().returncode, 0)
        self.readelf("libc.so.6", "2.36")
        self.assertNotEqual(self.verify().returncode, 0)

    def test_macos_rejects_homebrew_load_commands(self):
        self.tool("file", "printf 'Mach-O 64-bit executable arm64\\n'")
        self.tool("otool", "printf 'kitmux:\\n /usr/lib/libSystem.B.dylib (compatibility version 1)\\n'")
        self.assertEqual(self.verify("darwin").returncode, 0)
        self.tool("otool", "printf 'kitmux:\\n /opt/homebrew/lib/libjson-c.dylib (compatibility version 1)\\n'")
        self.assertNotEqual(self.verify("darwin").returncode, 0)
        self.tool("otool", "printf 'kitmux:\\n /usr/lib/libSystem.B.dylib (compatibility version 1)\\n'")
        self.tool("xcrun", "printf 'minos 26.0\\n'")
        self.assertNotEqual(self.verify("darwin").returncode, 0)

    def test_failed_inspectors_cannot_approve_a_binary(self):
        self.tool("readelf", "exit 77")
        self.assertNotEqual(self.verify().returncode, 0)
        self.readelf("libc.so.6", "2.35")
        self.tool("readelf", "if [ \"$1\" = -d ]; then exit 0; else exit 77; fi")
        self.assertNotEqual(self.verify().returncode, 0)
        self.tool("file", "printf 'Mach-O 64-bit executable arm64\\n'")
        self.tool("otool", "exit 77")
        self.assertNotEqual(self.verify("darwin").returncode, 0)

    def test_wrong_format_and_filename_architecture_are_rejected(self):
        self.tool("file", "printf '/tmp/arm64/kitmux: POSIX shell script, ASCII text executable\\n'")
        self.assertNotEqual(self.verify().returncode, 0)
        self.tool("file", "printf 'ELF 64-bit LSB pie executable, x86-64\\n'")
        self.assertNotEqual(self.verify().returncode, 0)


if __name__ == "__main__":
    unittest.main()
