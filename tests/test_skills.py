import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class SkillTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.home = self.root / "home"
        self.home.mkdir()
        self.source = self.root / "meeting-notes"
        self.source.mkdir()
        self.write_skill("First instructions")
        (self.source / "references").mkdir()
        (self.source / "references/context.txt").write_text("Useful context")

    def write_skill(self, body):
        (self.source / "SKILL.md").write_text(
            "---\nname: meeting-notes\ndescription: Take useful meeting notes\n---\n" + body + "\n")

    def cli(self, *args, ok=True):
        p = subprocess.run([BINARY, "--home", str(self.home), *map(str, args)],
                           text=True, capture_output=True, timeout=20)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0)
        return p

    def catalog(self):
        return json.loads((self.home / ".config/kitmux/catalog.json").read_text())

    def test_local_install_preserves_package_and_starts_unassigned(self):
        script = self.source / "run.sh"
        marker = self.root / "must-not-execute"
        script.write_text(f"#!/bin/sh\ntouch '{marker}'\n")
        script.chmod(0o755)
        self.cli("skill", "add", self.source)
        skill = self.catalog()["skills"]["meeting-notes"]
        installed = Path(skill["path"])
        self.assertTrue(installed.is_relative_to(self.home / ".local/share/kitmux/skills"))
        self.assertNotEqual(installed, self.source)
        self.assertEqual((installed / "references/context.txt").read_text(), "Useful context")
        self.assertEqual((installed / "SKILL.md").read_bytes(), (self.source / "SKILL.md").read_bytes())
        self.assertTrue((installed / "run.sh").stat().st_mode & 0o100)
        self.assertFalse(marker.exists())
        self.assertEqual(skill["agents"], {})
        self.assertEqual(len(skill["hash"]), 64)

    def test_symlink_escape_is_rejected_without_installation(self):
        outside = self.root / "outside"
        outside.write_text("private fixture")
        (self.source / "escape").symlink_to(outside)
        p = self.cli("skill", "add", self.source, ok=False)
        self.assertIn("symlink", p.stderr.lower())
        self.assertEqual(outside.read_text(), "private fixture")
        path = self.home / ".config/kitmux/catalog.json"
        if path.exists():
            self.assertEqual(self.catalog()["skills"], {})

    def test_invalid_frontmatter_and_duplicate_keys_are_rejected(self):
        for contents in ["No YAML header\n", "---\nname: BAD NAME\ndescription: x\n---\nx",
                         "---\nname: meeting-notes\nname: other\ndescription: x\n---\nx",
                         "---\nname: meeting-notes\ndescription: [not, scalar]\n---\nx"]:
            (self.source / "SKILL.md").write_text(contents)
            self.cli("skill", "add", self.source, ok=False)

    def test_update_preview_apply_and_rollback(self):
        self.cli("skill", "add", self.source)
        original = self.catalog()["skills"]["meeting-notes"]
        catalog_before = (self.home / ".config/kitmux/catalog.json").read_bytes()
        self.write_skill("Revised instructions")
        preview = self.cli("skill", "update", "meeting-notes", "--preview")
        self.assertIn("meeting-notes", preview.stdout)
        self.assertEqual((self.home / ".config/kitmux/catalog.json").read_bytes(), catalog_before)
        self.cli("skill", "update", "meeting-notes", "--apply")
        updated = self.catalog()["skills"]["meeting-notes"]
        self.assertNotEqual(updated["hash"], original["hash"])
        self.assertIn("Revised", (Path(updated["path"]) / "SKILL.md").read_text())
        self.assertIn("First", (Path(original["path"]) / "SKILL.md").read_text())
        self.cli("skill", "rollback", "meeting-notes")
        rolled_back = self.catalog()["skills"]["meeting-notes"]
        self.assertEqual(rolled_back["path"], original["path"])

    def test_remove_keeps_source_and_cleans_group_membership(self):
        self.cli("skill", "add", self.source)
        self.cli("skill", "group", "add", "communication")
        self.cli("skill", "group", "include", "communication", "skill:meeting-notes")
        self.cli("skill", "remove", "meeting-notes")
        self.assertTrue((self.source / "SKILL.md").exists())
        self.assertEqual(self.catalog()["skills"], {})
        self.assertEqual(self.catalog()["groups"]["skill:communication"]["members"], [])

    def test_git_install_pins_commit_and_ignores_untracked_files(self):
        subprocess.run(["git", "init", "-q", str(self.source)], check=True)
        subprocess.run(["git", "-C", str(self.source), "add", "."], check=True)
        subprocess.run(["git", "-C", str(self.source), "-c", "user.name=Fixture",
                        "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"], check=True)
        commit = subprocess.check_output(["git", "-C", str(self.source), "rev-parse", "HEAD"], text=True).strip()
        (self.source / "untracked.txt").write_text("Do not install")
        self.cli("skill", "add", self.source, "--ref", commit)
        skill = self.catalog()["skills"]["meeting-notes"]
        self.assertEqual(skill["revision"], commit)
        self.assertFalse((Path(skill["path"]) / "untracked.txt").exists())
        self.assertFalse((Path(skill["path"]) / ".git").exists())

    def test_unsafe_subdirectory_and_git_ref_are_rejected(self):
        self.cli("skill", "add", self.source, "--path", "../outside", ok=False)
        self.cli("skill", "add", self.source, "--ref", "--upload-pack=evil", ok=False)

    def test_add_does_not_ignore_preview_option(self):
        self.cli("skill", "add", self.source, "--preview", ok=False)

    def test_git_revision_update_is_recorded_when_content_is_identical(self):
        subprocess.run(["git", "init", "-q", str(self.source)], check=True)
        subprocess.run(["git", "-C", str(self.source), "add", "."], check=True)
        commit = ["git", "-C", str(self.source), "-c", "user.name=Fixture",
                  "-c", "user.email=fixture@example.invalid", "commit", "-qm"]
        subprocess.run([*commit, "first"], check=True)
        branch = subprocess.check_output(["git", "-C", str(self.source), "branch", "--show-current"], text=True).strip()
        self.cli("skill", "add", self.source, "--ref", branch)
        original = self.catalog()["skills"]["meeting-notes"]
        subprocess.run([*commit, "metadata-only", "--allow-empty"], check=True)
        newer = subprocess.check_output(["git", "-C", str(self.source), "rev-parse", "HEAD"], text=True).strip()
        self.cli("skill", "update", "meeting-notes", "--apply")
        updated = self.catalog()["skills"]["meeting-notes"]
        self.assertEqual(updated["revision"], newer)
        self.assertEqual(updated["hash"], original["hash"])

    def test_rollback_rejects_modified_cached_revision(self):
        self.cli("skill", "add", self.source)
        original = self.catalog()["skills"]["meeting-notes"]
        self.write_skill("Revised instructions")
        self.cli("skill", "update", "meeting-notes", "--apply")
        (Path(original["path"]) / "SKILL.md").write_text(
            "---\nname: meeting-notes\ndescription: Take notes\n---\nTampered\n")
        before = (self.home / ".config/kitmux/catalog.json").read_bytes()
        result = self.cli("skill", "rollback", "meeting-notes", ok=False)
        self.assertIn("modified", result.stderr)
        self.assertEqual((self.home / ".config/kitmux/catalog.json").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
