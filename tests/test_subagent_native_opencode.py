"""Real OpenCode user-profile discovery without a model or authenticated account."""
import json
import os
import shutil
import subprocess
import time
import unittest
from urllib.parse import quote

from native_clients.common import Sandbox
from native_clients.opencode import OpenCodeServer


@unittest.skipUnless(os.environ.get("KITMUX_NATIVE_TESTS") == "1" and shutil.which("opencode"),
                     "opt-in native OpenCode subagent qualification")
class OpenCodeSubagentNativeTests(unittest.TestCase):
    def test_markdown_and_json_profiles_disappear_and_return(self):
        sandbox = Sandbox("opencode")
        self.addCleanup(sandbox.close)
        sandbox.catalog_path.write_text(json.dumps({"version": 1, "revision": 0,
                                                   "groups": {}, "mcps": {}, "skills": {}}))
        subprocess.run(["git", "init", "-q", str(sandbox.home)], env=sandbox.env,
                       check=True, capture_output=True, timeout=10)
        markdown = sandbox.home / "review.md"
        markdown.write_text("Review changes without editing files.\n")
        sandbox.kitmux("subagent", "add", "kitmux-md-probe", "--from", str(markdown),
                       "--description", "Kitmux Markdown native qualification")
        definition = sandbox.home / "review.json"
        definition.write_text(json.dumps({"description": "Kitmux JSON native qualification",
            "mode": "subagent", "prompt": "Review changes without editing files.", "disabled": True,
            "permissions": [{"action": "*", "resource": "*", "effect": "deny"}]}))
        sandbox.kitmux("subagent", "add", "kitmux-json-probe", "--from", str(definition),
                       "--format", "opencode-json")
        profiles = {"kitmux-md-probe", "kitmux-json-probe"}
        for profile in sorted(profiles):
            sandbox.kitmux("subagent", "enable", profile, "--agent", "opencode")
        sandbox.kitmux("sync", "--apply", "--agent", "opencode")
        server = OpenCodeServer(sandbox)
        self.addCleanup(server.close)
        endpoint = "/api/agent?location%5Bdirectory%5D=" + quote(str(sandbox.home))

        def expect(available):
            deadline = time.monotonic() + 8
            ids = set()
            while time.monotonic() < deadline:
                rows = server.request("GET", endpoint)["data"]
                ids = {row["id"] for row in rows}
                # OpenAPI readiness precedes location-scoped profile loading.
                if "build" in ids and ids & profiles == available:
                    for row in rows:
                        if row["id"] in available:
                            self.assertEqual(row["mode"], "subagent")
                    return
                time.sleep(.1)
            self.fail(f"OpenCode did not discover expected fixture profiles: {available}, observed {ids & profiles}")

        expect(profiles)
        for profile in sorted(profiles):
            sandbox.kitmux("subagent", "disable", profile, "--agent", "opencode")
            expect(profiles - {profile})
            sandbox.kitmux("subagent", "enable", profile, "--agent", "opencode")
            expect(profiles)
