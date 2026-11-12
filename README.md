# Kitmux

A C terminal manager for MCP servers, Agent Skills, and custom subagents across
**Codex, Claude Code, and OpenCode**. Organize resources into groups, choose which
clients receive them, and manage availability from an interactive terminal or
ordinary commands.

**v0.5.0 · C17 · [MIT](LICENSE)**

- Manage MCP servers, complete skill packages, and custom subagent definitions.
- Organize each resource view into groups, with explicit controls for each coding client.
- Import existing setup, preview native changes, detect conflicts, and restore.
- Use a keyboard interface with selectors, colors, filtering, and multiple selection.
- Read installed skill documents, subagent instructions, and MCP descriptions in the terminal.

[Contributing](CONTRIBUTING.md) · [Security reporting](SECURITY.md) ·
[Architecture](docs/IMPLEMENTATION.md) · [Third-party notices](THIRD_PARTY.md)

See [v0.5 notes](docs/V0.5.md) for document views and binary distribution, and
[v0.4 notes](docs/V0.4.md) for scoped groups and legacy catalog migration.

## Download and install

Download a prebuilt executable from [Releases](https://github.com/cleissonom/kitmux/releases).
The release targets macOS 15+ (Apple Silicon and Intel) and Linux with glibc 2.35+
(ARM64 and x86_64). Linux needs system terminfo data, commonly supplied by
`ncurses-base` (some terminals also need `ncurses-term`); Git is needed only for
repository skill downloads. No compiler,
Python, Node, or Homebrew libraries are required by the release executable.
Exact platform qualification is recorded in [the implementation notes](docs/IMPLEMENTATION.md).

This block downloads one pinned release, verifies its SHA256 checksum, and installs
to `~/.local/bin`. Set `KITMUX_INSTALL_DIR` to use another directory. An existing
regular executable is saved as `kitmux.previous`; symlink installations are left
for their original installer to manage.

<!-- kitmux-install -->
```sh
(
  set -eu
  kitmux_version=v0.5.0
  case "$(uname -s)" in
    Darwin) kitmux_os=darwin ;;
    Linux) kitmux_os=linux ;;
    *) echo "Unsupported operating system" >&2; exit 1 ;;
  esac
  case "$(uname -m)" in
    arm64|aarch64) kitmux_arch=arm64 ;;
    x86_64|amd64) kitmux_arch=x86_64 ;;
    *) echo "Unsupported architecture" >&2; exit 1 ;;
  esac
  kitmux_name="kitmux-${kitmux_version}-${kitmux_os}-${kitmux_arch}"
  kitmux_url="https://github.com/cleissonom/kitmux/releases/download/${kitmux_version}"
  kitmux_tmp=$(mktemp -d)
  kitmux_new= kitmux_backup=
  trap 'rm -rf "$kitmux_tmp"; [ -z "$kitmux_new" ] || rm -f "$kitmux_new"; [ -z "$kitmux_backup" ] || rm -f "$kitmux_backup"' EXIT
  trap 'exit 1' HUP INT TERM
  curl -fL --proto '=https' --tlsv1.2 "$kitmux_url/$kitmux_name.tar.gz" -o "$kitmux_tmp/$kitmux_name.tar.gz"
  curl -fL --proto '=https' --tlsv1.2 "$kitmux_url/SHA256SUMS" -o "$kitmux_tmp/SHA256SUMS"
  awk -v name="$kitmux_name.tar.gz" '$2 == name { print }' "$kitmux_tmp/SHA256SUMS" > "$kitmux_tmp/checksum"
  [ "$(wc -l < "$kitmux_tmp/checksum")" -eq 1 ] || { echo "Missing or ambiguous checksum" >&2; exit 1; }
  (cd "$kitmux_tmp"; if command -v sha256sum >/dev/null 2>&1; then sha256sum -c checksum; else shasum -a 256 -c checksum; fi)
  tar -xzf "$kitmux_tmp/$kitmux_name.tar.gz" -C "$kitmux_tmp"
  [ "$("$kitmux_tmp/$kitmux_name/kitmux" --version)" = "kitmux ${kitmux_version#v}" ]
  kitmux_dir=${KITMUX_INSTALL_DIR:-"$HOME/.local/bin"}
  mkdir -p "$kitmux_dir"
  [ ! -L "$kitmux_dir/kitmux" ] || { echo "Existing kitmux is a symlink; use its original installer" >&2; exit 1; }
  [ ! -e "$kitmux_dir/kitmux" ] || [ -f "$kitmux_dir/kitmux" ]
  if [ -f "$kitmux_dir/kitmux" ]; then
    [ ! -L "$kitmux_dir/kitmux.previous" ] && { [ ! -e "$kitmux_dir/kitmux.previous" ] || [ -f "$kitmux_dir/kitmux.previous" ]; } || {
      echo "Backup path must be absent or a regular file" >&2; exit 1;
    }
  fi
  kitmux_new=$(mktemp "$kitmux_dir/.kitmux.XXXXXXXX")
  install -m 755 "$kitmux_tmp/$kitmux_name/kitmux" "$kitmux_new"
  if [ -f "$kitmux_dir/kitmux" ]; then
    kitmux_backup=$(mktemp "$kitmux_dir/.kitmux.XXXXXXXX")
    cp -p "$kitmux_dir/kitmux" "$kitmux_backup"
    mv -f "$kitmux_backup" "$kitmux_dir/kitmux.previous"
    kitmux_backup=
  fi
  mv -f "$kitmux_new" "$kitmux_dir/kitmux"
  kitmux_new=
  printf 'Installed %s at %s/kitmux\n' "$kitmux_version" "$kitmux_dir"
)
```

Add the install directory to PATH in your shell configuration, then start Kitmux:

```sh
export PATH="$HOME/.local/bin:$PATH"
kitmux --version
kitmux
```

To upgrade, change `kitmux_version` to a published tag and rerun the block. Downloads
and verification finish before replacing the executable. Restart coding clients
after upgrading so managed MCP bridges use the new binary. For binary rollback,
run `mv "$HOME/.local/bin/kitmux.previous" "$HOME/.local/bin/kitmux"` (adjust the
directory if overridden). This does not roll back catalog or native configuration;
use the documented migration backup and `restore` workflow when needed. v0.4 and
v0.5 use catalog schema 3; versions before v0.4 cannot read that schema.

### Install and import with your local agent

Copy this prompt into your local coding agent:

```text
Install Kitmux from https://github.com/cleissonom/kitmux/releases and import my
existing setup. Read the README for the selected stable release. Resolve one
release tag, select the executable for this machine, verify its SHA256 checksum,
and install it at ~/.local/bin/kitmux using the documented atomic upgrade and
backup procedure. Keep an existing newer version; report incompatible platforms
or symlink installations instead of replacing another installer's files.
Ensure Kitmux is reachable on PATH and verify its version.

Detect existing user configurations for Codex, Claude Code, and OpenCode, including
their skill/subagent directories. For the detected clients, run import --preview,
inspect the result, and apply the import of supported MCPs, skills, and subagents.
If an older catalog requires migration, use group migrate --preview then --apply.
Preserve client-specific definitions, credentials, disabled states, unrelated
settings, and restore records. Retain project/plugin resources in their native
scope. Do not force conflicts, rewrite the private catalog/journal, or use refresh
to silently adopt conflicting external edits. Report skipped or conflicting items.

Verify with kitmux --version, status, doctor, and sync --check for each imported
client. If a check reports pending changes, inspect sync --preview and apply only
changes consistent with preserving my existing setup. Report imported resource
counts, unresolved conflicts, and client reload steps. Never print credentials
or complete native configuration files. Proceed with installation and supported
imports; ask only if resolving a conflict requires a choice about my setup.
```

## Build and run from source

Requirements: C17 compiler, Make, pkg-config, json-c, libyaml, ncursesw, and
OpenSSL libcrypto. Python 3.11+ is needed for tests only. On macOS, install build
dependencies with `brew install pkgconf json-c libyaml ncurses openssl`. On
Ubuntu/Debian use `build-essential`, `pkg-config`, `libjson-c-dev`, `libyaml-dev`,
`libncurses-dev`, and `libssl-dev`.

```sh
make
./build/kitmux --help
make install PREFIX="$HOME/.local"
```

Without installation, substitute `./build/kitmux` for `kitmux` in the examples.

Run `kitmux --help` for commands. Without a TTY, the application prints
help instead of waiting for input. `NO_COLOR=1` or `--no-color` disables colors.
Reload clients after upgrading so new processes use the installed version.
Older catalogs can be inspected before migration. Preview and apply the group
upgrade before editing groups:

```sh
kitmux group migrate --preview
kitmux group migrate --apply
```

Migration splits mixed groups by resource type, preserves their availability,
and backs up the original catalog before writing schema version 3. Empty legacy
groups become MCP groups, as listed in the preview. Keep v0.4 for subsequent
recovery; older executables cannot read schema version 3.

## First use

Import previews are read-only. Importing adopts existing user MCP definitions,
filesystem skill packages, and custom subagent definitions while retaining native
settings without displaying credentials. A shared MCP name can have different
commands, environment, or working directory per client; Kitmux keeps those
execution variants under one catalog ID.

```sh
kitmux import --from codex,claude,opencode --preview
kitmux import --from codex,claude,opencode --apply
kitmux status
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
kitmux mcp group add communication
kitmux mcp group include communication teams slack
kitmux mcp group assign communication --agents codex,claude,opencode
kitmux mcp group enable communication
```

The example assumes those server IDs exist. Imported resources preserve their
existing direct agent assignments; use `mcp inherit ID --agent AGENT` to let group
membership control them instead.

For a new catalog, preview and apply native configuration once:

```sh
kitmux sync --preview
kitmux sync --apply
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
| `1`–`5`, Tab | MCPs, Skills, Subagents, Clients, Status |
| Arrows, `j`/`k` | Move selection |
| Enter | Expand/collapse a group; open a resource document |
| Left/Right | Collapse/expand a group |
| `v` | Show group details or open a resource document |
| Space | Select multiple rows |
| `/`, Esc | Filter; clear filter or cancel |
| `n`, `x`, `d` | Add, edit, remove |
| `g` | Create a group inside the current resource tab |
| `p` on Subagents | Duplicate a definition with a new ID |
| `e`, `a`, `m` | Toggle, assign clients, edit group memberships |
| `r` | Reconnect selected MCP |
| `s`, `i` | Preview/apply native sync or import |
| `u`, `b` | Preview/apply skill update or roll back |
| `c`, `?`, `q` | Check configuration, help, quit |
| `f` on Status | Refresh the captured process-status snapshot |

The interface needs at least 74 columns and 20 rows. It handles terminal resize,
normal exit, and interruption without leaving echo disabled.
Resource documents support arrows or `j`/`k`, Page Up/Down, Space, Home/End or
`g`/`G`; Enter, Esc, or `q` returns to the selected row. Headings and fenced code
receive terminal formatting, while other Markdown remains readable as text.
Skills show the installed `SKILL.md`, including documents larger than 32 KiB.
Subagents show shared instructions and labeled effective native variants. MCPs
show their description, transport, mode, and availability without endpoint
arguments, environment values, or native authentication fields. Opening details
does not run servers or change configuration.
Groups show their matching resources as indented rows; uncategorized resources
appear under Ungrouped. Search finds resources even inside collapsed groups.
An item appearing in several groups is one resource: edits apply everywhere,
and bulk actions process it once. Removing a group preserves its resources.
Groups can show mixed effective client availability when member overrides differ.
Local MCP forms offer direct/managed mode selection. Reconnect asks which client
to target and shows its result. Status is a captured snapshot; press `f` to query
again.

## Custom subagents

Press `3` for Subagents, select a profile, and press `a` to choose its clients.
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
kitmux subagent group add engineering
kitmux subagent group include engineering code-reviewer
kitmux subagent group assign engineering --agents codex,claude,opencode
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
kitmux mcp disable teams --agent claude
kitmux mcp inherit teams --agent claude
kitmux mcp group disable communication
kitmux status --json
```

Assignment shares definitions. It does not merge accounts, authentication stores,
permissions, or conversation state. These controls are not a sandbox against an
agent that also has ordinary filesystem/shell access.

## MCP servers

Register an existing command or an HTTP endpoint:

```sh
kitmux mcp add example --command /absolute/path/to/mcp-server -- --stdio
kitmux mcp add docs --url https://example.test/mcp
kitmux mcp edit example --cwd /absolute/project/path
kitmux mcp edit example --description-file ./server-guide.md
kitmux mcp edit docs --description "Search the team's documentation"
kitmux mcp reconnect example --agent codex
kitmux mcp status example --agent codex --json
kitmux mcp remove example
```

Registration does not install the MCP server. Direct mode leaves connection
ownership with the client; reconnect returns an actionable instruction when
Kitmux cannot control that client connection. HTTP/OAuth remains direct.
Use one of `--description TEXT` or `--description-file FILE` with add/edit, including
native `--from` provisioning. The TUI form offers text, Markdown file, keep/skip,
and clear choices. Descriptions remain local catalog metadata; imports and refresh
preserve authored text. Existing entries without documentation show a fallback.

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
kitmux mcp status example --agent codex
kitmux status --json
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
kitmux skill add /absolute/path/to/meeting-notes
kitmux skill add owner/repository --path skills/meeting-notes --ref COMMIT
kitmux skill group add communication
kitmux skill group include communication meeting-notes
kitmux skill update meeting-notes --preview
kitmux skill update meeting-notes --apply --ref NEW_COMMIT
kitmux skill rollback meeting-notes
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
kitmux doctor
kitmux restore --agent claude --preview
kitmux restore --agent claude --apply
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
The [v0.5 release notes](docs/V0.5.md) describe the current release.

## Contributing and license

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, test expectations,
bug reports, and pull requests. GitHub Actions runs normal and sanitizer suites
on the macOS/Linux architecture matrix; installed-client qualification remains
an explicit local check.

Kitmux is available under the [MIT license](LICENSE). Vendored code retains its
upstream notices, documented in [THIRD_PARTY.md](THIRD_PARTY.md).
