# Kitmux

A C terminal manager for MCP servers, Agent Skills, and custom subagents across
**Codex, Claude Code, and OpenCode**. Organize resources into groups, choose which
clients receive them, and manage availability from an interactive terminal or
ordinary commands.

**v0.3.1 · C17 · [MIT](LICENSE)**

- Manage MCP servers, complete skill packages, and custom subagent definitions.
- Share resources through groups, with explicit controls for each coding client.
- Import existing setup, preview native changes, detect conflicts, and restore.
- Use a keyboard interface with selectors, colors, filtering, and multiple selection.

[Contributing](CONTRIBUTING.md) · [Security reporting](SECURITY.md) ·
[Architecture](docs/IMPLEMENTATION.md) · [Third-party notices](THIRD_PARTY.md)

Version 0.3 adds custom subagent creation, import, editing, duplication, groups,
and per-client availability. Native variants retain each client's model, tools,
permissions, and other settings. The local MCP bridge and skill package controls
remain available with the compatibility limits below.
Version 0.3.1 adds native MCP source files, skill source rebinding, and explicit
refresh of externally edited imports for configuration-as-code integrations.

## Build and run

From a source ZIP, extract the archive and open a terminal in the `kitmux`
directory. Build on the destination computer so the executable matches its
architecture and installed libraries.

Requirements: a C17 compiler, Make, pkg-config, json-c, libyaml, ncurses, and
OpenSSL libcrypto. Git is needed for repository skill downloads. Python 3.11+
is needed for tests only; the application has no Python or Node runtime.

On macOS, dependencies can be installed with:

```sh
brew install pkgconf json-c libyaml ncurses openssl
```

On Ubuntu/Debian, the corresponding development packages are `build-essential`,
`pkg-config`, `libjson-c-dev`, `libyaml-dev`, `libncurses-dev`, and `libssl-dev`.
This implementation has been exercised on macOS arm64; Linux still needs platform
qualification.

```sh
make
./build/kitmux
```

The executable stays under `build/`. Optional installation:

```sh
make install PREFIX="$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
```

Add that PATH entry to your shell configuration if needed. Examples below that
use `kitmux` assume it is installed on PATH; otherwise use `./build/kitmux`.

Run `./build/kitmux --help` for commands. Without a TTY, the application prints
help instead of waiting for input. `NO_COLOR=1` or `--no-color` disables colors.
Reload clients after upgrading so new processes use the installed version.
V0.2 catalogs load automatically. Subagent data upgrades the catalog and ownership
journal to version 2; older Kitmux executables reject those files. Keep the newer
executable for restore and recovery.

## First use

Import previews are read-only. Importing adopts existing user MCP definitions,
filesystem skill packages, and custom subagent definitions while retaining native
settings without displaying credentials. A shared MCP name can have different
commands, environment, or working directory per client; Kitmux keeps those
execution variants under one catalog ID.

```sh
./build/kitmux import --from codex,claude,opencode --preview
./build/kitmux import --from codex,claude,opencode --apply
./build/kitmux status
```

To adopt deliberate external edits to already-owned entries, preview and apply
`import --refresh --from CLIENT`. Refresh retains the native file bytes and the
original restore baseline while updating Kitmux's selected client snapshots.
Ordinary import, sync, and restore continue to reject conflicting edits. Refresh
rejects drifted managed MCP bridges; reconcile their native bridge configuration
before importing so the bridge cannot become its own upstream.

Skill discovery includes legacy Codex `.codex/skills`, shared `.agents/skills`,
Claude `.claude/skills`, and OpenCode `.config/opencode/skills` plus its compatible
shared roots. Identical complete packages are deduplicated while native client
visibility and alias locations are retained. Existing leaf symlinks are adopted
without changing them during import; later sync manages the selected aliases,
and restore reinstates their original targets. Canonical source packages remain
untouched. Bundled `.system`, synced collections, and plugin caches are excluded.

Real directories in owned discovery locations, symlinked discovery roots, and
different package contents sharing one skill ID require explicit migration.
Import fails clearly rather than selecting a conflicting package silently.
Native Codex skill-disable controls remain in force; re-enabling a blocked skill
requires changing that specific native override before retrying sync.

Create a group and assign it to any subset of the clients:

```sh
./build/kitmux group add communication
./build/kitmux group include communication mcp:teams mcp:slack
./build/kitmux group assign communication --agents codex,claude,opencode
./build/kitmux group enable communication
```

The example assumes those server IDs exist. Imported resources preserve their
existing direct agent assignments; use `mcp inherit ID --agent AGENT` to let group
membership control them instead.

For a new catalog, preview and apply native configuration once:

```sh
./build/kitmux sync --preview
./build/kitmux sync --apply
```

Use `--agent codex`, `--agent claude`, or `--agent opencode` to adopt one client.
After adoption, catalog mutations automatically sync **only adopted clients**.
An external edit produces a conflict; Kitmux retains your desired catalog change
and reports that native sync is pending. Resolve the conflict before retrying.

Native configuration changes may require reloading the client or starting a new
session. `status` reports desired availability alongside observed managed bridge
processes. Native connection state and client tool refresh remain unverified;
writing configuration is not proof of a connection.
For automation, `kitmux sync --check [--agent CLIENT]` performs the same read-only
planning and returns zero only when selected projections are current and no
journal recovery is pending. A planned change, conflict, or pending journal
returns nonzero. `--check` cannot be combined with `--apply`.

## Configuration as code

Use native MCP source files when provisioning options such as tool allowlists,
timeouts, or per-client environment values:

```sh
kitmux mcp add docs --from ./docs-codex.json --format codex
kitmux mcp edit docs --from ./docs-claude.json --format claude
kitmux skill update reviewer --source ./skills/reviewer --preview
kitmux skill update reviewer --source ./skills/reviewer --apply
```

For every format, the MCP source is **one JSON object**, not a complete client
configuration: the contents of Codex's `mcp_servers.ID`, Claude's `mcpServers.ID`,
or OpenCode's `mcp.servers.ID`. For example, a Codex source can contain
`{"url":"https://example.invalid/mcp","enabled_tools":["search"]}`. Local Codex
and Claude definitions use a command string plus an optional `args` array;
OpenCode uses a command array. A source must select exactly one command or URL.

Native edits change only that client's execution and options. Existing global
availability, client assignments, groups, and runtime mode remain in force;
new resources start unassigned. At an unchanged command/argument/working-directory
or URL endpoint,
source fields recursively merge with that client's existing native options,
preserving omitted credentials and local settings. A changed endpoint replaces
its native snapshot using only the supplied source; old credentials are not
transferred. Other clients retain their own variants. Keep credential-bearing
source files private and provide credentials again only for the intended endpoint.

Skill updates with `--source DIRECTORY` retain policy and rollback history and
rebind the update source even when package contents match. Repeated identical
native MCP, subagent, and skill updates leave the catalog revision unchanged.
An explicit new skill source resets the previous source's subdirectory and Git
ref unless `--path` or `--ref` is supplied again.
`status --json` exposes each resource's global `enabled` and raw `assignments`
alongside effective availability; it omits MCP options and credentials.

## Keyboard controls

| Key | Action |
|---|---|
| `1`–`6`, Tab | Groups, MCPs, Skills, Subagents, Clients, Status |
| Arrows, `j`/`k` | Move selection |
| Enter | Show details |
| Space | Select multiple rows |
| `/`, Esc | Filter; clear filter or cancel |
| `n`, `x`, `d` | Add, edit, remove |
| `p` on Subagents | Duplicate a definition with a new ID |
| `e`, `a`, `m` | Toggle, assign agents, select group members |
| `r` | Reconnect selected MCP |
| `s`, `i` | Preview/apply native sync or import |
| `u`, `b` | Preview/apply skill update or roll back |
| `c`, `?`, `q` | Check configuration, help, quit |
| `f` on Status | Refresh the captured process-status snapshot |

The interface needs at least 74 columns and 20 rows. It handles terminal resize,
normal exit, and interruption without leaving echo disabled.
Local MCP forms offer direct/managed mode selection. Reconnect asks which client
to target and shows its result. Status is a captured snapshot; press `f` to query
again.

## Custom subagents

Press `4` for Subagents, select a profile, and press `a` to choose its clients.
Space toggles a checkbox; Enter saves. `e` controls global availability. Subagents
use the same group union, explicit per-client off, and global off rules as skills
and MCPs. Assignments control future native discovery, not running tasks.

Import only existing subagent definitions:

```sh
kitmux subagent import --from codex,claude,opencode --preview
kitmux subagent import --from codex,claude,opencode --apply
kitmux subagent list
kitmux subagent show code-reviewer
```

Create a portable profile from a plain Markdown instructions file:

```sh
kitmux subagent add code-reviewer --from ./review-instructions.md \
  --description "Review code changes for correctness and regressions"
kitmux group add engineering
kitmux group include engineering subagent:code-reviewer
kitmux group assign engineering --agents codex,claude,opencode
kitmux sync --preview
kitmux sync --apply
```

New profiles begin unassigned. Portable profiles contain a description and
instructions; the target client supplies its normal native defaults. They do not
translate models, grant tools, or preload resources merely through group membership.

```sh
kitmux subagent disable code-reviewer --agent claude
kitmux subagent inherit code-reviewer --agent claude
kitmux subagent edit code-reviewer --from ./updated-instructions.md
kitmux subagent duplicate code-reviewer release-reviewer
kitmux subagent remove release-reviewer
```

Duplicating copies the definition and native variants but starts with no client
assignments or group memberships. Editing shared instructions preserves native
options. Use `--format codex`, `--format claude`, or `--format opencode` when the
source is a native TOML or Markdown definition. A native-format edit supplies a
variant for that client. Imported profiles keep separate native variants under
one ID; assigning an imported profile to a client with no variant reports it as
incompatible until an appropriate native variant is supplied.

User definition paths are `~/.codex/agents/*.toml`, `~/.claude/agents/*.md`, and
`~/.config/opencode/agents/*.md`; OpenCode v2 also supports `agents` entries in
its JSON/JSONC configuration. Imports preserve originals for `restore` and detect
conflicting external edits. V0.3 manages flat, regular-file collections; nested
collections, symlinks, and filename/native-name mismatches need explicit migration.
Changing an adopted or projected OpenCode profile between Markdown and JSON is
rejected before saving; edit it in its current format. Project and plugin
definitions keep native precedence.
After applying changes, refresh the client or start a new session; disabling a
definition does not terminate a running subagent or erase its context.

## Availability rules

- A globally disabled resource is off for every client.
- An explicit per-client `off` wins over group membership.
- Otherwise, direct `on` or membership in any enabled assigned group makes it
  available. Multiple groups do not duplicate a resource.
- `inherit` removes a per-client override. No assignment means off.

```sh
./build/kitmux mcp disable teams --agent claude
./build/kitmux mcp inherit teams --agent claude
./build/kitmux group disable communication
./build/kitmux status --json
```

Assignment shares definitions. It does not merge accounts, authentication stores,
permissions, or conversation state. These controls are not a sandbox against an
agent that also has ordinary filesystem/shell access.

## MCP servers

Register an existing command or an HTTP endpoint:

```sh
./build/kitmux mcp add example --command /absolute/path/to/mcp-server -- --stdio
./build/kitmux mcp add docs --url https://example.test/mcp
./build/kitmux mcp edit example --cwd /absolute/project/path
./build/kitmux mcp reconnect example --agent codex
./build/kitmux mcp status example --agent codex --json
./build/kitmux mcp remove example
```

Registration does not install the MCP server. Direct mode leaves connection
ownership with the client; reconnect returns an actionable instruction when
Kitmux cannot control that client connection. HTTP/OAuth remains direct.

Imported endpoints cannot be changed to a different command/URL under the same
ID. Add a new ID and review its settings; authentication from the previous
endpoint must not silently follow it.

### Managed local bridge

For a compatible local server, use `--mode managed` when adding or editing it.
Native settings then launch a Kitmux bridge under that server's original name.
The bridge starts its own upstream process during capability discovery, observes
catalog policy changes, and accepts `mcp reconnect` through a private local socket.
It stays registered while disabled, returns an empty tool list, and rejects stale
calls. Removing the resource removes its owned native entry.

Each downstream connection has a separate upstream process. Reconnect fails
interrupted calls with an unknown-completion error and never replays them.
Only tracked child process groups are stopped. There is no permanent daemon.

Inspect processes without starting or reconnecting them:

```sh
./build/kitmux mcp status example --agent codex
./build/kitmux status --json
```

Authenticated local queries report each bridge PID, its actual upstream PID,
pending requests, and protocol. `waiting` means the client has not initialized;
`idle` means initialized with no upstream; `running` means a live upstream was
observed; `disabled` means the bridge has observed off policy. An `inactive` result
means no bridge was found; stale sockets are `unreachable`, and a mix of reachable and unreachable
connections is `partial`. Direct connections remain `unverified`. These process
observations do not certify that the client's tool list refreshed.
Queries have a 500ms/64-endpoint budget per MCP/client; JSON marks incomplete
scans with `scan_complete: false` and excludes unqueried endpoints from counts.

**Supported bridge contract:** newline-delimited stdio, tools only, MCP revisions
`2024-11-05`, `2025-03-26`, `2025-06-18`, and `2025-11-25`. Unknown version offers
in legacy initialization negotiate to `2025-11-25`; modern discovery probes
receive method-not-found so clients can fall back. The 2026 stateless lifecycle,
resources, prompts, roots, sampling, and elicitation are not supported. Upstream
servers may select any of the supported legacy revisions.

Native lifecycle qualification uses isolated Codex app-server, Claude Code headless,
and OpenCode private-server surfaces with read-only fixture tools and synthetic
providers. It does not certify arbitrary servers or authenticated integrations.
Keep session-bound integrations such as ai-memory direct. See the
[compatibility evidence and qualification limits](docs/RUNTIME_COMPATIBILITY.md)
and [exact native client test results](docs/NATIVE_CLIENT_QUALIFICATION.md).

Cancellation is forwarded only for outstanding calls. Cancelled calls retain a
bounded record until upstream completion or reconnect, so late results cannot be
mistaken for another call. They count toward the 64-request limit. Only correlated
progress is forwarded. Upstream initialization instructions are not surfaced by
the lazy tools-only bridge; use direct mode for servers that require them.

Upstream stderr is discarded to avoid exposing credentials. Limits include a
1 MiB protocol message, 64 pending requests, and bounded startup/shutdown. Control
sockets require a private, sufficiently short state path; the CLI reports an
actionable error if the platform socket-path limit is exceeded.

## Skills

```sh
./build/kitmux skill add /absolute/path/to/meeting-notes
./build/kitmux skill add owner/repository --path skills/meeting-notes --ref COMMIT
./build/kitmux group include communication skill:meeting-notes
./build/kitmux skill update meeting-notes --preview
./build/kitmux skill update meeting-notes --apply --ref NEW_COMMIT
./build/kitmux skill rollback meeting-notes
```

Complete skill packages are copied into a SHA256-addressed store outside agent
discovery roots. Downloads use Git objects rather than a checkout; package scripts,
checkout filters, and hooks do not execute during installation. Source revisions
are pinned and packages start unassigned. Updates preserve agent choices and show
the revision plus changed files before applying.

The installer checks standard YAML metadata, rejects package-internal symlinks and
special files, and bounds files, total size, directory depth, and entry count.
Use import to adopt existing native leaf aliases with a restore record; directory
aliases or unsupported collection layouts require deliberate migration.

Client adapters own only their projections and visibility entries. OpenCode's
compatibility discovery from Claude/shared skill directories is handled through
explicit skill permissions; disabling suggestions alone is insufficient. A skill
already loaded into a conversation remains in its context until a fresh session.

Removing a skill detaches its owned projections after sync and cleans group
membership. Source repositories and cached revisions remain. Cache pruning and
full textual update diffs are not implemented yet.

### Kitmux usage skill

The included [Kitmux skill](skills/kitmux/SKILL.md) teaches agents the CLI workflow,
group policy, safe imports, reconnect limits, and native refresh requirements.
Its default invocation policy permits automatic selection for relevant tasks.
Use the same package for all three clients:

```sh
kitmux skill add /path/to/kitmux/skills/kitmux
kitmux skill enable kitmux --agent codex
kitmux skill enable kitmux --agent claude
kitmux skill enable kitmux --agent opencode
```

These direct assignments make the guide available by default across the clients;
normal per-client controls can disable it. Clients may need a refresh or new
session to discover a newly installed skill.

## Configuration safety and scope

Kitmux manages **user-level** Codex, Claude Code, and OpenCode configuration.
Project and organization settings retain their native precedence and can change
effective availability. OpenCode integration targets the v2 schema. Native
configuration syntax and conflicts are checked; unsupported constructs produce
an error instead of a whole-file rewrite.

Imported Codex stdio definitions can switch between direct and managed mode while
preserving native policy and comments. Changes to multiline command/argument
values or unsupported TOML key layouts fail closed and retain the native file.

Owned entries have before/after records, while unrelated settings and JSONC/TOML
comments are preserved. Writes use a catalog lock, atomic replacement, private
permissions, and a journal for native updates. Partial multi-client application
is recoverable; it is not claimed to be one atomic transaction.

```sh
./build/kitmux doctor
./build/kitmux restore --agent claude --preview
./build/kitmux restore --agent claude --apply
```

Restore checks for concurrent edits and restores owned entries/projections to
their original state. It never overwrites newer unrelated changes. Restored
clients leave automatic management until explicitly adopted again with sync.

Paths honor XDG overrides for Kitmux's own files:

```text
~/.config/kitmux/catalog.json
~/.local/share/kitmux/skills/
~/.local/state/kitmux/adapters.json
~/.local/state/kitmux/r/
```

The catalog and ownership journal can contain private native settings. Keep them
local; do not publish them or add them to a dotfiles repository. Normal status and
preview output omit command arguments, environment values, and native auth fields.

## Development and verification

```sh
make test
make sanitize
```

Optional installed-client qualification uses isolated homes and synthetic loopback
providers without live accounts:

```sh
make test-native
```

See [native client qualification](docs/NATIVE_CLIENT_QUALIFICATION.md) for exact
tested releases and surfaces. Missing clients are reported as skipped tests.

Tests use temporary homes and stub processes. `--home DIR` requires an existing
absolute directory and isolates all Kitmux/native test paths beneath it without
changing the shell's HOME. No live client configuration is used by the suite.

The test suite covers policy precedence, all eight agent assignments, malformed
input, native preservation/conflicts, skill provenance/update/rollback, runtime
reconnect/cancellation boundaries, and terminal restoration. Sanitizer builds use
AddressSanitizer and UndefinedBehaviorSanitizer.

See [implementation progress](docs/IMPLEMENTATION.md), the original
[product plan](docs/PLAN.md), the [concept diagram](docs/kitmux-concept.html), and
[third-party notices](THIRD_PARTY.md).
The [v0.3 work and verification record](docs/V0.3.md) describes the current release.

## Contributing and license

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, test expectations,
bug reports, and pull requests. The macOS GitHub Actions workflow runs the normal
and sanitizer suites; installed-client qualification remains an explicit local
check.

Kitmux is available under the [MIT license](LICENSE). Vendored code retains its
upstream notices, documented in [THIRD_PARTY.md](THIRD_PARTY.md).
