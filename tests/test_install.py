"""Execute the README installer against staged release downloads, never live homes."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ReleaseInstallTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="km-install-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.downloads = self.root / "downloads"
        self.commands = self.root / "commands"
        self.destination = self.root / "local bin"
        self.downloads.mkdir()
        self.commands.mkdir()
        self.environment = dict(os.environ, PATH=f"{self.commands}:{os.environ['PATH']}",
                                KITMUX_INSTALL_DIR=str(self.destination),
                                FIXTURE_DOWNLOADS=str(self.downloads), FIXTURE_OS="Linux",
                                FIXTURE_ARCH="x86_64", FIXTURE_DOWNLOAD_LOG=str(self.root / "urls"))
        self.command("uname", '#!/bin/sh\ncase "$1" in -s) echo "$FIXTURE_OS";; -m) echo "$FIXTURE_ARCH";; esac\n')
        self.command("curl", '''#!/usr/bin/env python3
import os, pathlib, shutil, sys
args = sys.argv[1:]
url = next(arg for arg in args if arg.startswith("https://"))
with open(os.environ["FIXTURE_DOWNLOAD_LOG"], "a") as log:
    log.write(url + "\\n")
source = pathlib.Path(os.environ["FIXTURE_DOWNLOADS"]) / url.rsplit("/", 1)[1]
if not source.is_file():
    sys.exit(22)
shutil.copyfile(source, args[args.index("-o") + 1])
''')

    def command(self, name, body):
        target = self.commands / name
        target.write_text(body)
        target.chmod(0o755)

    def release(self, platform="linux", architecture="x86_64", version="0.5.0", executable=None):
        stem = f"kitmux-v0.5.0-{platform}-{architecture}"
        archive = self.downloads / f"{stem}.tar.gz"
        if executable is None:
            executable = f'#!/bin/sh\nprintf "kitmux {version}\\n"\n'.encode()
        with tarfile.open(archive, "w:gz") as package:
            entry = tarfile.TarInfo(f"{stem}/kitmux")
            entry.mode = 0o755
            entry.size = len(executable)
            package.addfile(entry, io.BytesIO(executable))
        checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
        (self.downloads / "SHA256SUMS").write_text(f"{checksum}  {archive.name}\n")
        return executable

    def install(self, success=True):
        readme = (ROOT / "README.md").read_text()
        self.assertTrue("<!-- kitmux-install -->" in readme, "README must provide the release installer")
        section = readme.split("<!-- kitmux-install -->", 1)[1]
        script = section.split("```sh\n", 1)[1].split("```", 1)[0]
        result = subprocess.run(["/bin/sh"], input=script, text=True, capture_output=True,
                                env=self.environment, timeout=20, cwd=self.root)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def existing(self):
        self.destination.mkdir()
        installed = self.destination / "kitmux"
        installed.write_bytes(b"previous executable\n")
        installed.chmod(0o755)
        return installed

    def test_each_supported_platform_downloads_one_tag_and_installs_executable(self):
        for system, machine, platform, architecture in (
                ("Linux", "x86_64", "linux", "x86_64"),
                ("Linux", "aarch64", "linux", "arm64"),
                ("Darwin", "x86_64", "darwin", "x86_64"),
                ("Darwin", "arm64", "darwin", "arm64")):
            with self.subTest(system=system, machine=machine):
                self.environment.update(FIXTURE_OS=system, FIXTURE_ARCH=machine)
                expected = self.release(platform, architecture)
                self.install()
                installed = self.destination / "kitmux"
                self.assertEqual(installed.read_bytes(), expected)
                self.assertTrue(os.access(installed, os.X_OK))
        urls = (self.root / "urls").read_text().splitlines()
        self.assertEqual(len(urls), 8)
        self.assertTrue(all(url.startswith("https://github.com/cleissonom/kitmux/releases/download/v0.5.0/")
                            for url in urls))

    def test_upgrade_preserves_previous_executable_for_rollback(self):
        installed = self.existing()
        previous = installed.read_bytes()
        expected = self.release()
        self.install()
        self.assertEqual(installed.read_bytes(), expected)
        self.assertEqual((self.destination / "kitmux.previous").read_bytes(), previous)

    def test_checksum_mismatch_preserves_installed_executable(self):
        installed = self.existing()
        self.release()
        (self.downloads / "SHA256SUMS").write_text(
            f"{'0' * 64}  kitmux-v0.5.0-linux-x86_64.tar.gz\n")
        self.install(success=False)
        self.assertEqual(installed.read_bytes(), b"previous executable\n")
        self.assertFalse((self.destination / "kitmux.previous").exists())

    def test_missing_checksum_entry_preserves_installed_executable(self):
        installed = self.existing()
        self.release()
        (self.downloads / "SHA256SUMS").write_text(f"{'0' * 64}  other.tar.gz\n")
        self.install(success=False)
        self.assertEqual(installed.read_bytes(), b"previous executable\n")

    def test_download_failure_preserves_installed_executable(self):
        installed = self.existing()
        self.install(success=False)
        self.assertEqual(installed.read_bytes(), b"previous executable\n")

    def test_wrong_version_does_not_replace_installed_executable(self):
        installed = self.existing()
        self.release(version="0.4.0")
        self.install(success=False)
        self.assertEqual(installed.read_bytes(), b"previous executable\n")

    def test_unsupported_system_and_architecture_fail_before_downloading(self):
        for key, value in (("FIXTURE_OS", "Windows_NT"), ("FIXTURE_ARCH", "riscv64")):
            with self.subTest(key=key):
                self.environment.update(FIXTURE_OS="Linux", FIXTURE_ARCH="x86_64")
                self.environment[key] = value
                self.install(success=False)
                self.assertFalse((self.root / "urls").exists())
                self.assertFalse(self.destination.exists())

    def test_installed_symlink_is_preserved(self):
        self.destination.mkdir()
        target = self.root / "owned elsewhere"
        target.write_text("external executable\n")
        installed = self.destination / "kitmux"
        installed.symlink_to(target)
        self.release()
        self.install(success=False)
        self.assertTrue(installed.is_symlink())
        self.assertEqual(target.read_text(), "external executable\n")

    def test_install_failure_preserves_installed_executable(self):
        installed = self.existing()
        self.release()
        self.command("install", "#!/bin/sh\nexit 1\n")
        self.install(success=False)
        self.assertEqual(installed.read_bytes(), b"previous executable\n")
        self.assertFalse(list(self.destination.glob(".kitmux.*")))

    def test_backup_directory_and_symlink_reject_upgrade_without_writing_through(self):
        installed = self.existing()
        self.release()
        backup = self.destination / "kitmux.previous"
        backup.mkdir()
        self.install(success=False)
        self.assertEqual(installed.read_bytes(), b"previous executable\n")
        self.assertEqual(list(backup.iterdir()), [])
        backup.rmdir()
        external = self.root / "external backups"
        external.mkdir()
        backup.symlink_to(external, target_is_directory=True)
        self.install(success=False)
        self.assertTrue(backup.is_symlink())
        self.assertEqual(list(external.iterdir()), [])
        self.assertEqual(installed.read_bytes(), b"previous executable\n")

    def test_documented_install_import_check_and_restore_with_real_binary(self):
        binary = Path(os.environ.get("KITMUX_BINARY", ROOT / "build/kitmux"))
        self.release(executable=binary.read_bytes())
        self.install()
        home = self.root / "fixture-home"
        home.mkdir()
        codex = home / ".codex/config.toml"
        codex.parent.mkdir()
        codex.write_text('[mcp_servers.docs]\nurl = "https://example.invalid/mcp"\n'
                         'enabled = false\n')
        package = home / "source/review"
        package.mkdir(parents=True)
        (package / "SKILL.md").write_text("---\nname: review\ndescription: Review code\n---\nReview safely.\n")
        skills = home / ".claude/skills"
        skills.mkdir(parents=True)
        (skills / "review").symlink_to(package)
        profiles = home / ".config/opencode/agents"
        profiles.mkdir(parents=True)
        profile = profiles / "reviewer.md"
        profile.write_text("---\ndescription: Review changes\nmode: subagent\n---\nReview changes carefully.\n")
        original_codex, original_profile = codex.read_bytes(), profile.read_bytes()

        def cli(*args):
            result = subprocess.run([str(self.destination / "kitmux"), "--home", str(home), *args],
                                    text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

        cli("import", "--from", "codex,claude,opencode", "--preview")
        catalog = home / ".config/kitmux/catalog.json"
        self.assertFalse(catalog.exists())
        cli("import", "--from", "codex,claude,opencode", "--apply")
        data = json.loads(catalog.read_text())
        self.assertIn("docs", data["mcps"])
        self.assertEqual(data["mcps"]["docs"]["agents"]["codex"], "off")
        self.assertIn("review", data["skills"])
        self.assertIn("reviewer", data["subagents"])
        cli("status")
        cli("doctor")
        for client in ("codex", "claude", "opencode"):
            cli("sync", "--preview", "--agent", client)
            cli("sync", "--apply", "--agent", client)
            cli("sync", "--check", "--agent", client)
        self.assertEqual(codex.read_bytes(), original_codex)
        self.assertEqual(profile.read_bytes(), original_profile)
        cli("restore", "--apply")
        self.assertEqual(codex.read_bytes(), original_codex)
        self.assertEqual(profile.read_bytes(), original_profile)
        self.assertEqual((skills / "review").readlink(), package)
