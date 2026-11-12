# Architecture and verification

Kitmux 0.5.0 is a C17 terminal application for MCP servers, Agent Skills, custom
subagents, groups, and assignments to Codex, Claude Code, and OpenCode. See the
[README](../README.md) for setup and commands and [v0.5 notes](V0.5.md) for the
current release. Earlier behavior is documented in [v0.4 notes](V0.4.md), [v0.3 notes](V0.3.md),
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

V0.5 adds bounded resource documents independent of command capture: installed
`SKILL.md`, shared/effective native subagent instructions, and optional MCP Markdown
descriptions. The viewer sanitizes terminal controls, wraps Unicode text, and
reflows on resize. Its file inputs retain the 8 MiB limit; combined subagent
documents allow four such bodies plus 64 KiB of metadata. Basic heading/code
formatting is supported, not full CommonMark rendering. Viewing is read-only and
never launches an MCP or fetches remote content.

MCP descriptions are optional schema-3-compatible metadata. Text/file CLI options
and TUI edits validate input without changing execution variants; re-import,
refresh, and native updates preserve authored text or an explicit empty value.
Existing show/JSON contracts remain unchanged. A native description is adopted
only when local documentation is absent.

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
- `tui_details.c` selects allowlisted resource content; `tui_document.c` owns the
  read-only Unicode/Markdown pager. Apply previews retain their explicit review
  behavior and bounded command-output buffer.
- `scripts/*release*.sh` build pinned dependency sources in a private prefix,
  verify runtime linkage/architecture/platform minima, and package executables
  with licenses. The release workflow gates publication on every target and
  stages complete assets in a draft before publishing. Manual runs produce the
  same archives/checksum manifest as workflow artifacts.

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

## V0.5 verification and release gates

Local qualification on 12 November 2026:

| Platform / command | Result |
|---|---|
| macOS arm64: `make -j4 test` | 274 passed; 6 opt-in native cases skipped; 280 discovered |
| macOS arm64: `make -j4 sanitize` | 274 passed; 6 opt-in cases skipped; ASan/UBSan enabled |
| Ubuntu 22.04 arm64: `make -j4 BUILD=build-linux PYTHON=python3.11 test` | 274 passed; 6 opt-in cases skipped; 280 discovered |
| Ubuntu 22.04 arm64: `ASAN_OPTIONS=detect_leaks=0 make -j4 BUILD=build-linux-sanitize SANITIZE_BUILD=build-linux-sanitize PYTHON=python3.11 sanitize` | 274 passed; 6 opt-in cases skipped; ASan/UBSan enabled |
| arm64 release builds on macOS and Ubuntu 22.04: `make release VERSION=v0.5.0` | Both versioned archives built; runtime linkage, architecture, and minimum-platform verification passed |
| Packaged executables: `sh scripts/smoke-release.sh BINARY` | Import, skill hashing, managed MCP reconnect, terminal restoration, xterm/tmux startup passed on both systems |
| Packaged macOS executable: `KITMUX_BINARY=build-release-darwin-arm64/kitmux PYTHONPATH=tests python3 -m unittest test_install.ReleaseInstallTests.test_documented_install_import_check_and_restore_with_real_binary -v` | Passed using staged downloads and an isolated home |

Linux qualification ran in Ubuntu 22.04 containers under a process reaper. Local
LeakSanitizer ptrace scans were prohibitively slow in the container, so the local
Linux sanitizer run disabled leak detection only; hosted CI retains default
ASan/UBSan settings. An initial deadline-sensitive runtime test exceeded its
wall-clock assertion while two dependency compilations saturated the host; the
complete suite passed after those builds finished without relaxing its deadline.

The Linux archive passed smoke tests in a fresh Ubuntu 22.04/glibc 2.35 runtime
containing terminal data and Python fixtures but no development libraries. On
macOS, the packaged executable also passed version/import/skill-hash/status checks
with Homebrew reads denied. This machine's pyenv Python depends on Homebrew
gettext, preventing the entire Python smoke harness from starting inside that
sandbox; its ordinary fixture smoke passed separately. Hosted release tests use
the setup-python interpreter and deny Homebrew reads for the full smoke harness.

Red tests preceded document display, MCP metadata validation/import/editing, and
release packaging behavior. Independent review added reproduced regressions for
metadata-only TUI edits of distinct imported client variants, failed binary
inspectors being swallowed by shell pipelines, filename-based architecture false
positives, and backup-directory/symlink handling in the installer. All were fixed
and focused re-review found no remaining blockers. Documentation/build-harness
changes used their applicable test-first exceptions.

The README installation block is executed directly by `tests/test_install.py`.
Staged fixtures cover all four platform mappings, checksum/download/version
failures, atomic upgrades and rollback, installation errors, and symlink/directory
preservation. A real executable completes installation, preview/apply import,
sync preview/apply/check, and restore with synthetic MCP/skill/subagent data.
Tests use temporary homes and do not mutate live client settings or install host
dependencies. Release/linkage tests reject missing notices, mismatched versions,
wrong architectures, newer platform requirements, and non-system dynamic libraries.

The table records local execution. The [CI workflow](../.github/workflows/ci.yml)
qualifies ordinary and sanitizer tests on all four native targets. The
[release workflow](../.github/workflows/release.yml) additionally verifies packaged
executables, combines four archives with `SHA256SUMS`, and publishes only when
every required job passes. Hosted results are available in the repository's
[Actions history](https://github.com/cleissonom/kitmux/actions).

## V0.4 verification baseline

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

Release targets are macOS 15+ and glibc 2.35+ Linux, each on arm64 and x86_64.
Source builds and fixture qualification do not certify arbitrary native client
versions or live services. The isolated Claude subagent native probe remains
macOS-only because it depends on `sandbox-exec`; its Linux equivalent is not
implemented. Cache pruning and full textual skill diffs remain follow-up work.
The historical v0.3
[architecture diagram](kitmux-concept.html) and [original design plan](PLAN.md)
provide further context; implemented contracts are defined by the current README,
code, and tests.
