# Architecture and verification

Kitmux 0.3.0 is a C17 terminal application for MCP servers, Agent Skills, custom
subagents, groups, and assignments to Codex, Claude Code, and OpenCode. See the
[README](../README.md) for setup and commands and [v0.3 notes](V0.3.md) for the
current release. Earlier behavior is documented in [v0.2 notes](V0.2.md) and the
[import compatibility record](IMPORT_FIX.md).

## Implementation boundaries

- `catalog.c`, `policy.c`, and `groups.c` own catalog validation, persistence,
  availability rules, and mixed groups. Global off and explicit client off win;
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
- `tui*.c` provide the ncurses interface; `main.c` routes equivalent CLI commands.

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

Subagent data upgrades catalogs and ownership journals to v2. Older Kitmux builds
reject v2 records; use v0.3 or newer for recovery. Native project and organization
settings keep their own precedence.

## Verification baseline

Release verification on macOS arm64, 11 November 2026:

| Command | Result |
|---|---|
| `make -j4 test` | 189 passed; 6 opt-in native cases skipped; 195 discovered |
| `make -j4 sanitize` | 189 passed; 6 opt-in cases skipped; ASan/UBSan enabled |
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

See [native client qualification](NATIVE_CLIENT_QUALIFICATION.md) for exact client
versions and tested surfaces and [runtime compatibility](RUNTIME_COMPATIBILITY.md)
for the supported MCP lifecycle.

## Qualification limits

User scope is the adapter boundary. Project/plugin/organization definitions retain
native precedence, loaded skills retain existing context, and removing a subagent
definition does not stop running work. Managed MCPs support tools-only legacy
stdio; unsupported capabilities and remote HTTP/OAuth require direct mode.

Linux is not yet qualified. Fresh-platform validation, package distribution, cache
pruning, and full textual skill diffs remain follow-up work. The current
[architecture diagram](kitmux-concept.html) and [original design plan](PLAN.md)
provide further context; implemented contracts are defined by the current README,
code, and tests.
