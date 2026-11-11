import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get('KITMUX_BINARY', str(Path(__file__).parents[1] / 'build/kitmux'))


class SubagentAdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name).resolve()

    def cli(self, *args, ok=True):
        result = subprocess.run([BINARY, '--home', str(self.home), *args],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def write(self, relative, text):
        path = self.home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def native(self, client, name='reviewer'):
        if client == 'codex':
            return self.write(f'.codex/agents/{name}.toml',
                f'# preserved codex\nname = "{name}"\ndescription = "Codex reviewer"\n'
                'developer_instructions = "Use Codex instructions."\nmodel = "gpt-test"\n')
        if client == 'claude':
            return self.write(f'.claude/agents/{name}.md',
                f'---\nname: {name}\ndescription: Claude reviewer\ntools: Read, Grep\n'
                'model: inherit\n---\nUse Claude instructions.\n')
        return self.write(f'.config/opencode/agents/{name}.md',
            '---\ndescription: OpenCode reviewer\nmode: subagent\nhidden: true\n'
            'permissions:\n  - action: edit\n    resource: "*"\n    effect: deny\n'
            '---\nUse OpenCode instructions.\n')

    def catalog(self):
        return json.loads((self.home / '.config/kitmux/catalog.json').read_text())

    def test_subagent_import_preview_is_read_only_and_preserves_variants(self):
        originals = {client: self.native(client) for client in ('codex', 'claude', 'opencode')}
        before = {client: path.read_bytes() for client, path in originals.items()}
        preview = self.cli('subagent', 'import', '--from', 'codex,claude,opencode', '--preview')
        self.assertIn('reviewer', preview.stdout)
        self.assertFalse((self.home / '.config/kitmux/catalog.json').exists())
        self.assertFalse((self.home / '.local/state/kitmux/adapters.json').exists())
        self.cli('subagent', 'import', '--from', 'codex,claude,opencode', '--apply')
        resource = self.catalog()['subagents']['reviewer']
        self.assertEqual(set(resource['native']), {'codex', 'claude', 'opencode'})
        self.cli('sync', '--apply')
        self.assertEqual({client: path.read_bytes() for client, path in originals.items()}, before)
        self.assertEqual(set(self.catalog()['mcps']), set())

    def test_disable_enable_remove_restore_exact_native_files(self):
        path = self.native('claude')
        original = path.read_bytes()
        self.cli('subagent', 'import', '--from', 'claude', '--apply')
        self.cli('subagent', 'disable', 'reviewer', '--agent', 'claude')
        self.assertFalse(path.exists())
        self.cli('subagent', 'enable', 'reviewer', '--agent', 'claude')
        self.assertEqual(path.read_bytes(), original)
        self.cli('subagent', 'remove', 'reviewer')
        self.assertFalse(path.exists())
        self.cli('restore', '--agent', 'claude', '--apply')
        self.assertEqual(path.read_bytes(), original)

    def test_import_includes_subagents_and_preserves_other_configuration(self):
        path = self.native('codex')
        config = self.write('.codex/config.toml', '# keep parent settings\nmodel = "existing-model"\n')
        self.cli('import', '--from', 'codex', '--apply')
        self.assertIn('reviewer', self.catalog()['subagents'])
        self.assertEqual(config.read_text(), '# keep parent settings\nmodel = "existing-model"\n')
        self.cli('subagent', 'disable', 'reviewer', '--agent', 'codex')
        self.assertFalse(path.exists())
        self.cli('restore', '--agent', 'codex', '--apply')
        self.assertTrue(path.exists())

    def test_owned_external_edit_blocks_disable_without_deleting_it(self):
        path = self.native('claude')
        self.cli('subagent', 'import', '--from', 'claude', '--apply')
        changed = path.read_text().replace('Use Claude', 'Owner changed')
        path.write_text(changed)
        result = self.cli('subagent', 'disable', 'reviewer', '--agent', 'claude', ok=False)
        self.assertIn('conflict', result.stderr.lower())
        self.assertEqual(path.read_text(), changed)

    def test_unowned_target_collision_is_not_overwritten(self):
        path = self.native('claude')
        source = self.write('portable.md', 'Review changes carefully.\n')
        self.cli('subagent', 'add', 'reviewer', '--from', str(source), '--description', 'Review code')
        self.cli('subagent', 'enable', 'reviewer', '--agent', 'claude')
        before = path.read_bytes()
        self.cli('sync', '--agent', 'claude', '--apply', ok=False)
        self.assertEqual(path.read_bytes(), before)

    def test_opencode_config_collision_prevents_a_shadowing_file(self):
        config = self.write('.config/opencode/opencode.jsonc',
                            '{"agents":{"reviewer":{"mode":"primary","prompt":"Owner prompt"}}}')
        original = config.read_bytes()
        source = self.write('portable.md', 'Review changes carefully.\n')
        self.cli('subagent', 'add', 'reviewer', '--from', str(source), '--description', 'Review code')
        self.cli('subagent', 'enable', 'reviewer', '--agent', 'opencode')
        self.cli('sync', '--agent', 'opencode', '--apply', ok=False)
        self.assertFalse((self.home / '.config/opencode/agents/reviewer.md').exists())
        self.assertEqual(config.read_bytes(), original)

    def test_new_json_variant_and_duplicate_project_as_json_entries(self):
        source = self.write('reviewer.json', '{"description":"Review code","mode":"subagent","prompt":"Review only"}')
        self.cli('subagent', 'add', 'reviewer', '--from', str(source), '--format', 'opencode-json')
        self.cli('subagent', 'enable', 'reviewer', '--agent', 'opencode')
        self.cli('sync', '--agent', 'opencode', '--apply')
        config = self.home / '.config/opencode/opencode.json'
        self.assertEqual(json.loads(config.read_text())['agents']['reviewer']['prompt'], 'Review only')
        self.assertFalse((self.home / '.config/opencode/agents/reviewer.md').exists())
        self.cli('subagent', 'duplicate', 'reviewer', 'reviewer-copy')
        self.cli('subagent', 'enable', 'reviewer-copy', '--agent', 'opencode')
        self.assertEqual(json.loads(config.read_text())['agents']['reviewer-copy']['prompt'], 'Review only')
        self.assertFalse((self.home / '.config/opencode/agents/reviewer-copy.md').exists())

    def test_edit_rejects_changing_a_managed_native_location_format(self):
        path = self.native('opencode')
        original = path.read_bytes()
        self.cli('subagent', 'import', '--from', 'opencode', '--apply')
        source = self.write('reviewer.json', '{"description":"Review code","mode":"subagent","prompt":"Review only"}')
        result = self.cli('subagent', 'edit', 'reviewer', '--from', str(source), '--format', 'opencode-json', ok=False)
        self.assertIn('format', result.stderr.lower())
        self.assertEqual(path.read_bytes(), original)

    def test_opencode_jsonc_disabled_import_is_preserved_and_restored(self):
        original = '{\n // preserve this comment\n "theme":"owner",\n "agents":{"reviewer":{' \
                   '"description":"Review code","mode":"subagent","prompt":"Review only",' \
                   '"disabled":true,"hidden":true,"model":"test/model"}}\n}\n'
        path = self.write('.config/opencode/opencode.jsonc', original)
        self.cli('subagent', 'import', '--from', 'opencode', '--apply')
        resource = self.catalog()['subagents']['reviewer']
        self.assertEqual(resource['agents']['opencode'], 'off')
        self.cli('sync', '--agent', 'opencode', '--apply')
        self.assertEqual(path.read_text(), original)
        self.cli('subagent', 'enable', 'reviewer', '--agent', 'opencode')
        self.assertFalse(json.loads(path.read_text().replace('// preserve this comment', ''))['agents']['reviewer'].get('disabled', False))
        self.cli('subagent', 'disable', 'reviewer', '--agent', 'opencode')
        self.assertTrue(json.loads(path.read_text().replace('// preserve this comment', ''))['agents']['reviewer']['disabled'])
        self.cli('restore', '--agent', 'opencode', '--apply')
        self.assertEqual(json.loads(path.read_text().replace('// preserve this comment', ''))['agents']['reviewer'],
                         json.loads(original.replace('// preserve this comment', ''))['agents']['reviewer'])
        self.assertIn('// preserve this comment', path.read_text())

    def test_reimport_keeps_disabled_owned_definition_and_restore_baseline(self):
        path = self.native('codex')
        original = path.read_bytes()
        self.cli('subagent', 'import', '--from', 'codex', '--apply')
        self.cli('subagent', 'disable', 'reviewer', '--agent', 'codex')
        self.cli('subagent', 'import', '--from', 'codex', '--apply')
        self.assertEqual(self.catalog()['subagents']['reviewer']['agents']['codex'], 'off')
        self.cli('restore', '--agent', 'codex', '--apply')
        self.assertEqual(path.read_bytes(), original)

    def test_symlink_source_and_directory_are_rejected_without_external_changes(self):
        external = self.write('external/reviewer.md', '---\nname: reviewer\ndescription: Review\n---\nReview.\n')
        root = self.home / '.claude/agents'
        root.mkdir(parents=True)
        (root / 'reviewer.md').symlink_to(external)
        before = external.read_bytes()
        self.cli('subagent', 'import', '--from', 'claude', '--apply', ok=False)
        self.assertEqual(external.read_bytes(), before)
        (root / 'reviewer.md').unlink()
        root.rmdir()
        root.symlink_to(external.parent)
        self.cli('subagent', 'import', '--from', 'claude', '--apply', ok=False)
        self.assertEqual(external.read_bytes(), before)

    def test_malformed_subagent_journal_paths_never_write_outside_discovery(self):
        path = self.native('claude')
        self.cli('subagent', 'import', '--from', 'claude', '--apply')
        journal = self.home / '.local/state/kitmux/adapters.json'
        state = json.loads(journal.read_text())
        records = state['subagent_files']
        record = next(iter(records.values()))
        escaped = self.write('owner.txt', 'do not change')
        state['subagent_files'] = {str(escaped): record}
        journal.write_text(json.dumps(state))
        self.cli('restore', '--agent', 'claude', '--apply', ok=False)
        self.assertEqual(escaped.read_text(), 'do not change')
        self.assertTrue(path.exists())

    def test_origin_without_native_variant_is_rejected_before_sync(self):
        path = self.native('claude')
        original = path.read_bytes()
        self.cli('subagent', 'import', '--from', 'claude', '--apply')
        catalog = self.home / '.config/kitmux/catalog.json'
        data = json.loads(catalog.read_text())
        data['subagents']['reviewer']['portable'] = True
        data['subagents']['reviewer'].pop('native')
        catalog.write_text(json.dumps(data))
        self.cli('sync', '--agent', 'claude', '--apply', ok=False)
        self.assertEqual(path.read_bytes(), original)

    def test_malformed_json_subagent_journal_is_not_restored(self):
        path = self.write('.config/opencode/opencode.json',
                          '{"agents":{"reviewer":{"description":"Review","mode":"subagent","prompt":"Review"}}}')
        self.cli('subagent', 'import', '--from', 'opencode', '--apply')
        original = path.read_bytes()
        journal = self.home / '.local/state/kitmux/adapters.json'
        state = json.loads(journal.read_text())
        state['files'][str(path)]['entries']['agents/reviewer']['before'] = '42'
        journal.write_text(json.dumps(state))
        self.cli('restore', '--agent', 'opencode', '--apply', ok=False)
        self.assertEqual(path.read_bytes(), original)

    def test_disabled_owned_json_target_rejects_a_new_shadowing_file(self):
        source = self.write('reviewer.json', '{"description":"Review","mode":"subagent","prompt":"Review"}')
        self.cli('subagent', 'add', 'reviewer', '--from', str(source), '--format', 'opencode-json')
        self.cli('subagent', 'enable', 'reviewer', '--agent', 'opencode')
        self.cli('sync', '--agent', 'opencode', '--apply')
        config = self.home / '.config/opencode/opencode.json'
        original = config.read_bytes()
        self.native('opencode')
        self.cli('subagent', 'disable', 'reviewer', '--agent', 'opencode', ok=False)
        self.assertEqual(config.read_bytes(), original)

    def test_pending_sync_accepts_before_and_after_but_rejects_unrelated_edit(self):
        path = self.native('claude')
        self.cli('subagent', 'import', '--from', 'claude', '--apply')
        journal = self.home / '.local/state/kitmux/adapters.json'
        state = json.loads(journal.read_text())
        state['pending'] = True
        record = state['subagent_files'][str(path)]
        record['previous'] = record['after']
        record.pop('after')
        journal.write_text(json.dumps(state))
        self.cli('subagent', 'disable', 'reviewer', '--agent', 'claude')
        self.assertFalse(path.exists())
        self.cli('sync', '--agent', 'claude', '--apply')
        self.assertFalse(json.loads(journal.read_text())['pending'])

    def test_opencode_primary_definitions_remain_unowned_beside_subagents(self):
        primary = self.write('.config/opencode/agents/writer.md',
                             '---\ndescription: Write\nmode: primary\n---\nWrite carefully.\n')
        original = primary.read_bytes()
        self.native('opencode')
        config = self.write('.config/opencode/opencode.jsonc',
                            '{"agents":{"build":{"mode":"primary","model":"owner/model"}}}')
        self.cli('subagent', 'import', '--from', 'opencode', '--apply')
        self.assertEqual(set(self.catalog()['subagents']), {'reviewer'})
        self.cli('subagent', 'disable', 'reviewer', '--agent', 'opencode')
        self.assertEqual(primary.read_bytes(), original)
        self.assertEqual(json.loads(config.read_text())['agents']['build']['model'], 'owner/model')

    def test_nested_native_collections_require_migration_before_import(self):
        for client in ('claude', 'opencode'):
            with self.subTest(client=client):
                flat = self.native(client)
                nested = flat.parent / 'team' / flat.name
                nested.parent.mkdir()
                flat.rename(nested)
                original = nested.read_bytes()
                result = self.cli('subagent', 'import', '--from', client, '--apply', ok=False)
                self.assertIn('nested', result.stderr.lower())
                self.assertIn('migrate', result.stderr.lower())
                self.assertEqual(nested.read_bytes(), original)
                self.assertFalse((self.home / '.config/kitmux/catalog.json').exists())

    def test_nested_definition_prevents_projection_over_a_hidden_collision(self):
        nested = self.write('.claude/agents/team/reviewer.md',
                            '---\nname: reviewer\ndescription: Nested reviewer\n---\nOwner instructions.\n')
        original = nested.read_bytes()
        source = self.write('portable.md', 'Review code.\n')
        self.cli('subagent', 'add', 'reviewer', '--from', str(source), '--description', 'Review code')
        self.cli('subagent', 'enable', 'reviewer', '--agent', 'claude')
        self.cli('sync', '--agent', 'claude', '--apply', ok=False)
        self.assertFalse((self.home / '.claude/agents/reviewer.md').exists())
        self.assertEqual(nested.read_bytes(), original)

    def test_nested_definition_prevents_a_false_successful_disable(self):
        flat = self.native('claude')
        original = flat.read_bytes()
        self.cli('subagent', 'import', '--from', 'claude', '--apply')
        nested = self.write('.claude/agents/team/reviewer.md', original.decode())
        result = self.cli('subagent', 'disable', 'reviewer', '--agent', 'claude', ok=False)
        self.assertIn('nested', result.stderr.lower())
        self.assertEqual(flat.read_bytes(), original)
        self.assertEqual(nested.read_bytes(), original)

    def test_nested_collections_do_not_block_mcp_only_sync(self):
        nested = self.write('.claude/agents/team/reviewer.md',
                            '---\nname: reviewer\ndescription: Review\n---\nReview.\n')
        original = nested.read_bytes()
        self.cli('mcp', 'add', 'example', '--command', '/usr/bin/true')
        self.cli('mcp', 'enable', 'example', '--agent', 'claude')
        self.cli('sync', '--agent', 'claude', '--apply')
        self.assertEqual(nested.read_bytes(), original)
        self.assertIn('example', json.loads((self.home / '.claude.json').read_text())['mcpServers'])

    def test_symlinked_collection_is_rejected_without_following_it(self):
        external = tempfile.TemporaryDirectory()
        self.addCleanup(external.cleanup)
        target = Path(external.name) / 'reviewer.md'
        target.write_text('---\nname: reviewer\ndescription: External\n---\nOwner instructions.\n')
        original = target.read_bytes()
        root = self.home / '.claude/agents'
        root.mkdir(parents=True)
        (root / 'team').symlink_to(target.parent)
        self.cli('subagent', 'import', '--from', 'claude', '--apply', ok=False)
        self.assertEqual(target.read_bytes(), original)
        self.assertFalse((self.home / '.config/kitmux/catalog.json').exists())

    def test_legacy_opencode_collection_blocks_import_and_false_disable(self):
        flat = self.native('opencode')
        self.cli('subagent', 'import', '--from', 'opencode', '--apply')
        original = flat.read_bytes()
        legacy = self.write('.config/opencode/agent/reviewer.md', original.decode())
        result = self.cli('subagent', 'import', '--from', 'opencode', '--preview', ok=False)
        self.assertIn('legacy', result.stderr.lower())
        self.assertIn('migrate', result.stderr.lower())
        self.cli('subagent', 'disable', 'reviewer', '--agent', 'opencode', ok=False)
        self.assertEqual(flat.read_bytes(), original)
        self.assertEqual(legacy.read_bytes(), original)

    def test_legacy_opencode_collection_does_not_block_mcp_only_sync(self):
        legacy = self.write('.config/opencode/agent/reviewer.md',
                            '---\ndescription: Review\nmode: subagent\n---\nReview.\n')
        original = legacy.read_bytes()
        self.cli('mcp', 'add', 'example', '--command', '/usr/bin/true')
        self.cli('mcp', 'enable', 'example', '--agent', 'opencode')
        self.cli('sync', '--agent', 'opencode', '--apply')
        self.assertEqual(legacy.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
