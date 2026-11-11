# Import compatibility and Kitmux usage skill

This historical v0.2 follow-up added per-client MCP execution variants and
filesystem skill imports. Custom subagent management is now implemented in
[v0.3](V0.3.md); the [README](../README.md) describes current behavior.

## Import behavior

Combined import previously rejected a shared MCP name when clients used
different commands. Import now retains one logical ID and
full per-client execution variants. Native names, commands, optional environment
and working directories, authentication settings, and direct/managed policy are
preserved. Re-importing an owned bridge retains its real upstream and off policy.

Import also previously omitted skills. It now discovers user packages in the
supported native roots, including legacy Codex leaf aliases, deduplicates whole
package hashes, and retains each client's discovery bindings and native on/off
state. Codex controls support exact name selectors and path selectors. Unsupported patterns or conflicting contents fail
explicitly instead of silently changing availability.

Malformed native snapshot maps are rejected before rendering. Imported catalog
data is committed before claiming native ownership, so a failed catalog save
cannot leave existing native definitions owned without their resource records.
Catalog and ownership writes are not a single filesystem transaction; retry an
import if a later ownership-journal write fails.
Use the current build to synchronize or restore catalogs containing execution
variants or imported discovery bindings; older builds do not understand them.

Import adopts existing leaf links without replacing them. Subsequent sync can
project immutable complete packages; restore returns original targets, including
relative targets. Canonical sources, bundled system collections, and unrelated
links remain outside that ownership. Existing real package directories in owned
discovery roots need explicit migration. Native Codex disable rules cannot be
silently bypassed by re-enabling a catalog entry.
An aborted apply can leave an unreferenced private package copy in the cache;
cache pruning remains deferred. Source packages and native aliases are preserved.

## Verification

Observed Red cases included the combined-client command conflict, missing skill
discovery, lost alias restore state, ignored native disable rules, malformed
discovery metadata, Codex name selectors, malformed native
snapshots, and ownership being claimed after a failed catalog commit. Tests were added
before behavioral fixes. Documentation, skill content, build-fixture dependencies,
and the updated import labels were non-behavioral exceptions.

- `make -j4 test`: **133 passed**, 3 opt-in native cases skipped (136 total).
- `make -j4 sanitize`: **133 passed**, 3 opt-in native cases skipped (136 total),
  under ASan/UBSan.
- `make test-native`: **3 passed**, no skips.
- `KITMUX_BINARY="$PWD/build/kitmux" python3 -m unittest discover -s tests -p 'test_tui.py' -v`:
  **8 passed** with import labels covering both MCPs and skills.
- An isolated-home smoke check installed the bundled Kitmux skill, enabled it for
  all three clients, and verified the complete package projections.

These results describe the import follow-up's verification baseline. Current
release results are recorded in [architecture and verification](IMPLEMENTATION.md).

## Adoption

Preview discovers supported resources without changing native configuration.
Apply records adopted resources and their originals; later catalog changes
synchronize adopted clients. Use an explicit preview before initial adoption:

```sh
kitmux import --from codex,claude,opencode --preview
kitmux import --from codex,claude,opencode --apply
```

The bundled [Kitmux skill](../skills/kitmux/SKILL.md) explains imports, assignments,
updates, restore, and refresh requirements. Follow the README to install it for
selected clients.
