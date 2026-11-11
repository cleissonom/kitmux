# Architecture and verification

Kitmux 0.4.0 is a C17 terminal application for MCP servers, Agent Skills, custom
subagents, groups, and assignments to Codex, Claude Code, and OpenCode. See the
[README](../README.md) for setup and commands and [v0.4 notes](V0.4.md) for the
current release. Earlier behavior is documented in [v0.3 notes](V0.3.md),
[v0.2 notes](V0.2.md), and the
[import compatibility record](IMPORT_FIX.md).

V0.3.1 adds native MCP JSON source provisioning, policy-preserving skill source
rebinding, explicit import refresh, read-only sync checks, and idempotent definition
edits. Source
provisioning retains each client's native options and preserves credentials only
at matching endpoints. The Codex adapter patches individual values and nested
tables while retaining comments, then validates the resulting definition.
Refresh changes snapshots without rewriting native bytes or losing the original
restore baseline. Drifted managed bridges remain rejected to protect upstreams.

V0.4 moves groups into their resource tabs and scopes group names, membership,
and actions to MCPs, Skills, or Subagents. Mixed legacy groups migrate explicitly
to matching scoped groups with private backups. Repeated rows share resource
identity, so overlapping memberships never duplicate a bulk resource action.

## Implementation boundaries

- `catalog.c`, `policy.c`, and `groups*.c` own catalog validation, persistence,
  availability rules, scoped groups, and legacy migration. Global off and explicit client off win;
  enabled groups combine, and unassigned resources remain off.
- `adapters*.c` preserve native configuration and own only adopted entries,
  projections, and definitions. Their journal records originals, current values,
  and pending writes for conflict detection and recovery.
- `skills*.c` install complete immutable packages from local directories or pinned
  Git objects. Updates preserve policy, and rollback retains previous packages.
- `subagents*.c` parse and render common instructions and native variants without
  translating away native model, tool, or permission settings.
- `runtime*.c` implement the optional local stdio MCP bridge and authenticated
  process controls. Direct integrations remain owned by each native client.
- `tui*.c` provide grouped resource trees in the ncurses interface; `main.c`
  routes equivalent CLI commands. Row identity distinguishes headers from
  resources, while resource identity deduplicates actions across memberships.

The build uses Make, json-c, libyaml, ncurses, and OpenSSL libcrypto. The vendored
TOML parser is documented in [third-party notices](../THIRD_PARTY.md). Python is
used for tests only.

## Persistence and recovery

Catalog mutations automatically synchronize clients previously adopted with
sync or import. Atomic file replacement and a pending ownership journal make
partial application recoverable; changes across multiple files are not a single
atomic transaction. Conflicting external edits are reported rather than replaced.
Restore recovers the adopted originals and releases the selected clients from
automatic management.

Scoped groups use catalog schema v3. Explicit migration splits legacy groups
without changing resource availability and backs up the original catalog before
replacement. Ownership journals retain their existing format. Older Kitmux
builds reject schema v3; keep v0.4 for recovery. Native project and organization
settings keep their own precedence.

## Verification baseline

Release verification on macOS arm64, 11 November 2026:

| Command | Result |
|---|---|
| `make -j4 test` | 236 passed; 6 opt-in native cases skipped; 242 discovered |
| `make -j4 sanitize` | 236 passed; 6 opt-in cases skipped; ASan/UBSan enabled |
| `make test-native` | 6 passed; no skips |

The tests use temporary homes, stub MCP processes, and synthetic local providers.
They cover all assignment subsets, policy precedence, imports, native setting
preservation, package updates and rollback, profile editing and duplication,
conflicts, pending recovery, malformed inputs, runtime lifecycle boundaries, and
terminal restoration. No live accounts or billed models are required.

Behavioral work used failing contract tests before implementation and regressions
before fixes. Important regressions include shared native names with different
commands, invalid native metadata, Unicode YAML offsets, JSON/Markdown projection
changes, nested discovery collisions, ambiguous interrupted calls, blocked
shutdown output, and incomplete process scans. Build scaffolding, protocol test
harnesses, and documentation were the test-first exceptions.

The v0.3.1 provisioning work began with failing subprocess contracts for native
MCP source loading, skill source rebinding, explicit drift refresh, and idempotent
profile edits. Additional regressions cover same-content source changes, stale
subdirectories, unsupported Codex values before commit, endpoint credential
isolation (including changed stdio working directories), nested native comments,
policy inspection without native secrets, preservation of raw assignments during
refresh, and read-only checks of planned projections and interrupted journals.
The focused command is `PYTHONPATH=tests python3 -m unittest test_provisioning
test_import_refresh test_sync_check -v`; all 25 provisioning cases pass.

V0.4 adds 10 scoped-group and migration cases and 12 terminal interaction cases.
Red failures preceded the new command/navigation behavior and fixes for adding a
resource inside a collapsed group, read-only implicit lists, backup location,
and failed migration exit status. Coverage includes original-byte backups,
native byte/mtime preservation, rejected cross-type mutations, overlapping
membership, group removal, search, cancellation, and terminal restoration.
Independent review confirmed backend migration preservation and all 26 terminal
workflows; the complete normal and sanitizer suites include them. Installed-client
qualification was rerun for this release as recorded above.

See [native client qualification](NATIVE_CLIENT_QUALIFICATION.md) for exact client
versions and tested surfaces and [runtime compatibility](RUNTIME_COMPATIBILITY.md)
for the supported MCP lifecycle.

## Qualification limits

User scope is the adapter boundary. Project/plugin/organization definitions retain
native precedence, loaded skills retain existing context, and removing a subagent
definition does not stop running work. Managed MCPs support tools-only legacy
stdio; unsupported capabilities and remote HTTP/OAuth require direct mode.

Linux is not yet qualified. Fresh-platform validation, package distribution, cache
pruning, and full textual skill diffs remain follow-up work. The historical v0.3
[architecture diagram](kitmux-concept.html) and [original design plan](PLAN.md)
provide further context; implemented contracts are defined by the current README,
code, and tests.
