# Kitmux — product and implementation plan

Original proposal, 11 November 2026. An initial implementation is available; see [README](../README.md) for the implemented behavior and compatibility limits and [implementation progress](IMPLEMENTATION.md) for verification. This document preserves the product direction. The initial build uses Make and json-c instead of the proposed CMake/yyjson combination.

## Product

A small C terminal application for choosing which MCP servers and Agent Skills are available to each coding agent. Manage a resource once, put it in groups, and assign those groups to any combination of Codex, Claude Code, and OpenCode.

Run `kitmux` for a keyboard-driven interface. Every action also has a scriptable command. The interface should take inspiration from [Mole](https://github.com/tw93/mole): clear menus, a visible selection, useful colors, search, multi-select, and concise results. Use an original implementation and visual identity.

**Recommendation:** build this. The useful differentiator is the combination of MCP lifecycle controls, skill installation, groups, and per-agent assignments. C is well suited to the terminal and process-management work. Cross-client compatibility and MCP protocol handling will require more care than drawing the interface.

Assumptions: macOS first, Linux next; one local OS user. Codex, Claude Code, and OpenCode are first-class clients in the initial release. “Agent” initially means a client installation, not every subagent or conversation. No web GUI or hosted service. The concept document is desktop-focused; mobile layouts are not required.

## Name

**Kitmux** is my first choice: a kit of tools and skills distributed across agents. The command is short, and the name does not tie the product to MCP alone. Alternatives: **Kitdock** for a home for agent tools, or **Latch** for availability controls. These are suggestions, not reserved names or trademark clearance.

## Core model

| Object | What it owns |
|---|---|
| MCP definition | Stable ID, native server name, transport, command and argument array or URL, working directory, environment references, per-client overrides, direct/managed mode |
| Skill | Stable ID, name, source repository/path, pinned revision, content hash, installed location, compatibility notes |
| Group | Name, description, resource memberships, enabled switch, assigned agent IDs |
| Agent | Client type, detected version, config scope and discovery paths, supported operations |
| Application record | Desired catalog revision, applied revision per client, owned entries/links, backup references, pending refresh state |

Groups may mix MCPs and skills. A resource can belong to several groups, but appears only once per agent. Groups do not nest in the first release.

Example only:

| Group | Contents | Enabled | Codex | Claude Code | OpenCode |
|---|---|---|---|---|---|
| Communication | Teams MCP, Slack MCP, meeting-notes skill | Yes | Yes | Yes | Yes |
| Engineering | GitHub MCP, code-review skill | Yes | Yes | No | Yes |
| Writing | humanizer skill | Yes | No | Yes | No |
| Research | search MCP, research-notes skill | No | Yes | Yes | Yes |

“Shared” means assigned to multiple agents. It does not mean sharing conversation state, accounts, OAuth tokens, or a single server process. A group with no assigned agents is saved but inactive everywhere.

### Availability rules

1. A global resource disable wins over every group.
2. A resource explicitly disabled for one agent stays off for that agent.
3. Otherwise, it is available when at least one enabled group containing it is assigned to that agent. A direct per-agent enable is an additional explicit assignment.
4. No assignment means off. A missing/incompatible installation produces a blocked result, not a false success.

Per-agent resource controls are `inherit`, `on`, and `off`. `on` adds a direct assignment but cannot override the global disable. `off` blocks all group assignments for that agent. Explain this in the details pane: “On through Communication”, “Off for Claude Code”, or “Still on through Engineering”. Turning off one group must not hide that another group still enables the resource.

Availability controls discovery and Kitmux-managed execution. It is not a security sandbox: an agent with ordinary shell access may still read files or launch a command through another route.

## Terminal experience

Home contains **Groups · MCPs · Skills · Agents · Status**. Open on Groups after the first import.

```text
  kitmux                            Codex · Claude Code · OpenCode

  Groups   MCPs   Skills   Agents   Status
  ───────────────────────────────────────────────────────────────
  GROUP              STATE   CODEX   CLAUDE   OPENCODE
  > Communication    ON      [x]     [x]      [x]
    Engineering      ON      [x]     [ ]      [x]
    Writing          ON      [ ]     [x]      [ ]
    Research         OFF     [x]     [x]      [x]

  Communication
    Teams              Enabled  Connected · managed
    Slack              Enabled  Reload required · direct
    meeting-notes      Enabled  Available to all 3 agents

  ↑↓ move  Enter open  Space select  e enable/disable  a assign
  / search  r reconnect  ? help  Esc back
```

This is a target-state mockup, not a running implementation. Group selection opens a focused screen for its contents and agent assignments. MCP rows expose add, edit, enable/disable, reconnect, remove, details, and bounded logs. Skills expose install, source details, assign, enable/disable, update preview, rollback, and remove. No sprawling configuration dashboard.

Use arrows and optional j/k; show the selected row with a marker as well as color. Enter opens or confirms the visible action; Space selects rows. Preserve the terminal on resize, interruption, and errors. Support `NO_COLOR`, plain output, ASCII fallback, and a compact layout at 80 columns. Non-TTY commands must never open a selector or wait for input.

Normal toggles apply immediately and show a concise result. Show a change preview for initial adoption, conflicting configuration, bulk removals, and updates. Avoid a mandatory approval wizard for every small action.

## Command surface

Proposed syntax, to be fixed before implementation:

```sh
kitmux
kitmux import --from codex,claude,opencode --preview
kitmux import --from codex,claude,opencode --apply
kitmux group add communication
kitmux group include communication mcp:teams mcp:slack skill:meeting-notes
kitmux group assign communication --agents codex,claude,opencode
kitmux group enable communication
kitmux group disable communication
kitmux mcp add teams --command /absolute/path/to/teams-mcp -- --stdio
kitmux mcp disable teams --agent claude
kitmux mcp inherit teams --agent claude
kitmux mcp reconnect teams --agent codex
kitmux mcp reconnect teams --agent opencode
kitmux skill add owner/repository --path skills/meeting-notes --ref COMMIT
kitmux skill update meeting-notes --preview
kitmux skill disable meeting-notes --agent claude
kitmux skill remove meeting-notes
kitmux status --json
kitmux doctor
```

Adding an MCP registers a command or URL; it does not silently install or execute its package. Downloaded skills start unassigned. The interactive flow can offer a group assignment as the next step. Secrets never belong in command examples, status JSON, or exports.

## What “active” means

Keep desired availability, runtime observation, and client refresh separate.

| Kind | Availability | Observable state |
|---|---|---|
| MCP | On/off for each agent | Stopped, starting, connected, reconnecting, failed, auth required, or unknown |
| Skill | On/off for each agent | Downloaded, projected into discovery, conflicting, incompatible, or unavailable |
| Either | Desired revision vs applied revision | Current, reload required, restart required, or unverified |

Do not display “connected” because a file was written. Direct connections owned by an uninstrumented client may have unknown status. A successfully parsed `tools/list` or supported client status API is evidence; a healthy PID alone is not.

### Two explicit MCP modes

**Direct mode — v0.1.** Kitmux manages native configuration; the client owns the connection. Prefer supported client operations and otherwise show the precise reload/restart action. `reconnect` performs a supported operation when available; if it cannot reach the running client, it returns `action_required` with instructions. Never claim a reconnect happened. No daemon is needed for this stage.

**Managed local stdio — v0.2.** The same C executable supplies a small per-user supervisor and per-connection stdio bridges. Each native server name points to its own `kitmux bridge` entry. The supervisor controls only the upstream child processes Kitmux owns. This enables live stop/reconnect without creating one giant aggregated MCP namespace.

Managed behavior:

- Off means no upstream MCP process. A small registered bridge may stay connected, expose an empty catalog, and reject stale calls.
- On allows upstream startup when the client discovers capabilities or calls a tool. Do not promise “first tool call only”: discovery can require starting the server to obtain schemas.
- Reconnect replaces the owned upstream connection and revalidates discovery. Do not replay an interrupted tool call automatically; it may already have changed external state.
- Disable blocks new calls immediately. Report in-flight operations; use bounded graceful shutdown and explicit cancellation policy. An operation already accepted upstream cannot be undone by the toggle.
- Publish catalog changes using the negotiated MCP version. A client may still need a refresh; report that separately from the successful upstream operation.
- Isolate every downstream connection. Do not pool Codex, Claude Code, and OpenCode, or separate client connections, onto one stateful upstream. Preserve environment, working directory, client identity, and protocol features within the supported contract. A client can multiplex conversations over one connection; this does not prove conversation-level isolation.
- Keep session-bound integrations, including ai-memory, direct until conversation/session identity is demonstrably preserved. A connection ID is not a conversation ID.
- Stop only tracked child process groups, with PID-reuse protection and bounded shutdown. Never kill an external browser, remote service, or unrelated shared container.

The bridge is a real MCP endpoint, not a byte-copy trick. Before production use, prove lifecycle/version negotiation, capability discovery, concurrent request IDs, cancellation, errors, pagination, and server-to-client requests. Advertise only supported capabilities. Servers needing unsupported features remain direct, with a visible compatibility reason. Pin a tested protocol/client compatibility matrix; do not assume one handshake or notification model works for every version.

Remote HTTP servers and OAuth remain native in both initial releases. A remote broker is a separate project only if a demonstrated requirement justifies it. This also keeps Executor optional: it might later be managed as one MCP entry, but it does not need to sit at the center of this tool.

## Skills

Store complete skill directories, including references, scripts, assets, and licenses, outside every agent auto-discovery root. A canonical location could be `~/.local/share/kitmux/skills/<id>/<revision>/`. The source revision and hash are immutable; updates create another revision.

Install from a Git repository plus subdirectory or an explicit local directory. Use the installed Git executable with an argument array and controlled options. Disable hooks, submodule recursion, and repository-supplied execution during acquisition. Do not execute skill scripts at install time. Reject unsafe paths, escaping symlinks, unsupported file types, excessive size, and malformed required metadata. Reuse a C YAML parser for frontmatter rather than attempting a partial regex parser. Show source and compatibility information before activation.

Client adapters expose only selected skills through owned links/copies or supported native visibility settings. Resolve the actual discovery paths for the installed client version. Current Codex documentation describes `~/.agents/skills` and repository `.agents/skills`; Claude Code documents `.claude/skills` and its own visibility controls. Shared aliases in an existing setup must be detected before applying per-client choices. If isolation cannot be enforced, report the conflict instead of claiming that the skill is disabled.

OpenCode also scans `.claude/skills` and `.agents/skills`, alongside its own skill directories. Removing an OpenCode link alone is insufficient. Its adapter must reconcile all discovered managed skill IDs and use targeted native `skill` deny rules where necessary, preserving unrelated permissions. `autoinvoke: false` only hides suggestions; it does not block explicit loading. Test effective permissions and duplicate IDs for the selected workspace. These requirements follow the [OpenCode skills documentation](https://opencode.ai/docs/skills/).

Use the standard skill directory format. Preserve client-specific metadata and flag incompatible extensions rather than silently translating instructions. Never edit native bundled skills, plugin caches, managed organization skills, or unowned files as part of a normal toggle.

Disabling a skill stops future discovery/invocation through the managed path. Instructions already loaded into a conversation remain in context; a fresh session is the reliable boundary. Native reload behavior varies by client and version.

Updates show the revision/content diff before moving owned projections. Rollback repoints them to the previous revision. Remove detaches owned projections and catalog memberships; shared source repositories are never deleted. Cached revisions are removed only by an explicit prune operation.

## Implementation in C

One C17 application, built with CMake. No Node/Python runtime for the product itself. MCP servers and skill scripts may still have their own runtimes.

| Concern | Initial choice | Reason |
|---|---|---|
| Interactive terminal | ncursesw | Input, colors, wide characters, resize handling |
| Catalog and command output | yyjson | One JSON representation for storage and scripting |
| Skill frontmatter | libyaml | Handle the actual YAML format safely |
| Persistence | Versioned JSON file + lock + atomic replacement | Inspectable, small, no database service |
| Skill acquisition | System Git via `posix_spawn` | Reuse installed authentication and revision handling |
| Native configuration | Versioned Codex/Claude/OpenCode adapters | Preserve each client's formats and semantics |
| Process/IPC control | POSIX in v0.1; consider libuv for v0.2 | Add the event-loop dependency only with the supervisor |
| Verification | CTest, compiler warnings, ASan/UBSan | Catch behavioral and memory errors |

Library choices are proposed, not installed. Pin audited releases during the initial spike. Native TOML editing is a specific feasibility item: prefer supported CLI operations, otherwise use a parser-assisted lossless edit of owned entries. Do not rewrite an entire user config through a lossy serializer or hand-edit TOML with regex. Fail closed on unsupported syntax until the adapter can preserve it.

OpenCode requires version detection: the current v2 schema uses `mcp.servers.<name>` and `disabled`, while older layouts differ. Its JSONC adapter must preserve comments and unknown fields, respect global/project precedence, and flag duplicate legacy entries. `/mcps` connection changes are transient; persistent choices require configuration changes. Preserve `_meta["ai.opencode/sessionID"]` unchanged when present in bridged calls; it is conversation metadata, not transport identity or authorization. See [OpenCode MCP](https://opencode.ai/docs/mcp-servers/) and [configuration](https://opencode.ai/docs/config/).

Suggested module boundaries: `cli`, `tui`, `catalog`, `policy`, `skills`, `adapters/codex`, `adapters/claude`, `adapters/opencode`, and later `runtime`. Keep shared business rules outside the TUI. Add abstractions only where the client adapters or tests actually need them.

Suggested persistent files:

```text
~/.config/kitmux/catalog.json       definitions, groups, assignments
~/.local/share/kitmux/skills/       pinned skill packages
~/.local/state/kitmux/              ownership, applied revisions, backups
<private runtime directory>/       supervisor socket and process state
```

Honor XDG overrides; choose a private macOS runtime fallback with owner-only permissions. Keep the catalog authoritative. Application records track observed application state rather than duplicating editable policy. Use a short lock, revision checking, temporary files on the same filesystem, fsync, and rename. Cross-client changes are not one filesystem transaction: journal per-client progress, recover after crashes, and visibly report partial application.

Never overwrite concurrent client edits: check the baseline before writing and present a rebase/conflict. Back up only affected files with private permissions and bounded retention. Preserve unknown configuration, native tool allowlists, approval policies, authentication references, hooks, and project trust choices. Keep secrets in native stores or existing environment/keychain references; catalog export must redact sensitive values. Redact and bound captured logs.

## Delivery sequence and acceptance

| Step | Deliverable | Acceptance gate |
|---|---|---|
| 0. Compatibility spike | Disposable fixtures for all three clients, skill discovery/isolation, native config mutation, stdio bridge experiment | Record tested client versions and unsupported operations; preserve an existing allowlist and unowned config; prove or reject live refresh without touching production credentials |
| 1. Registry and adapters | Import/adopt preview, JSON catalog, groups, per-agent bindings, status, native MCP add/remove/toggle | Applying twice is a no-op; unrelated settings survive; conflict and interrupted-write recovery work; pending reloads remain visible |
| 2. Skills and TUI — v0.1 | Pinned installation, projection, enable/disable, updates/rollback; keyboard menus and headless commands | A group can be shared by all three or any subset; disabled managed skills are undiscoverable and cannot be explicitly loaded through the native skill tool after the documented refresh, including OpenCode compatibility paths; full skill resources remain usable |
| 3. Local runtime — v0.2 | Optional supervisor, stable per-server bridges, live disable/reconnect, process diagnostics | All three supported clients can enable, call, disable, reject a stale call, and reconnect a stub MCP; independent connections stay isolated and per-call session metadata survives; no unowned process is stopped |
| 4. Distribution | macOS arm64/x86_64 builds, Linux qualification, release checksums, installation instructions | Fresh-machine smoke test, clean uninstall/restore, documented version support and limitations |

v0.1 is a useful early release, but does **not** complete the live reconnect requirement. The requested local-management feature set is complete only after step 3 passes for the declared client surfaces. Remote/session-bound direct integrations retain their explicit limitations.

### Test-first work

Before each behavioral change, add a focused failing contract test, implement it, refactor, then run the relevant suite. Initial red cases: a resource in two groups, explicit per-agent off, unowned config preservation, partial-apply recovery, and a skill selected for only one client despite existing shared symlinks. Exercise all eight assignment subsets of the three clients; specifically prove OpenCode does not inherit a skill enabled only for Claude Code or Codex.

Runtime tests use local stub MCP processes and synthetic credentials. Cover malformed/oversized messages, concurrent calls, upstream crashes, hanging shutdowns, cancellations, version differences, catalog changes, and ambiguous interrupted calls. Parser boundaries receive fuzzing and sanitizer runs. PTY tests cover keyboard actions, 80-column resize, no-color mode, non-TTY behavior, and terminal restoration.

Proposed commands once code exists: `cmake -S . -B build`, `cmake --build build`, and `ctest --test-dir build --output-on-failure`, plus a documented sanitizer build. These are future acceptance commands; they have not run for this planning-only task.

## Migration and external setup tools

Keep Kitmux in a standalone repository. Setup tools can install a pinned version
and optionally seed its catalog; adopted runtime choices remain owned by Kitmux.
An external template or configuration-management run must not silently re-enable
resources that Kitmux has disabled.

Migration starts read-only, preserving native server names and approval rules.
Adopt specific entries after a preview, with a restore record. Existing shared
skill aliases may require deliberate migration before client-specific activation
can be reliable. Session-bound integrations should remain direct and preserve
their client-specific invocation and session behavior.

Do not replace entire client configurations. Keep the scope focused on local
resource management rather than agent orchestration, a package marketplace,
cloud account sync, per-conversation policy, or an HTTP OAuth gateway.

## Material risks and rollback

- **Client changes:** versioned adapters, compatibility checks, unknown status when unobservable, direct mode fallback.
- **Partial or conflicting writes:** baseline checks, per-file atomic replacement, transaction journal, and restore of owned entries only.
- **Skill isolation leakage:** realpath/symlink checks and client discovery tests before claiming per-agent control.
- **MCP complexity:** bounded local stdio scope, a compatibility spike, conformance fixtures, and direct mode for unsupported capabilities.
- **Interrupted side effects:** no automatic replay during reconnect; surface unknown completion to the caller.
- **C memory/process safety:** bounded parsing, explicit ownership, sanitizers, fuzzing, and child-process tracking.

Rollback restores adopted native definitions and skill projections from their recorded baseline, after checking for later user edits. It stops only Kitmux-owned processes. Never restore an old whole-file backup over newer unrelated changes.

## Evidence and visual

The [concept diagram](kitmux-concept.html) shows the control model, example sharing matrix, terminal direction, and release split. It intentionally leaves protocol routing and recovery details in this plan.

Primary references checked while planning:

- [Mole](https://github.com/tw93/mole): interaction inspiration.
- [Codex MCP](https://learn.chatgpt.com/docs/extend/mcp): native server configuration and client behavior.
- [Codex app-server](https://learn.chatgpt.com/docs/app-server): supported integration APIs; their existence does not imply control over any arbitrary running Codex process.
- [Codex skills](https://learn.chatgpt.com/docs/build-skills): discovery paths, symlinks, refresh behavior.
- [Claude Code MCP](https://code.claude.com/docs/en/mcp): native scopes, reconnect and refresh behavior.
- [Claude Code skills](https://code.claude.com/docs/en/skills): discovery, visibility overrides, client-specific extensions and loaded context.
- [OpenCode MCP](https://opencode.ai/docs/mcp-servers/): v2 configuration, connection control and session metadata.
- [OpenCode skills](https://opencode.ai/docs/skills/): shared discovery roots and effective skill permissions.
- [OpenCode configuration](https://opencode.ai/docs/config/): JSONC and configuration scopes.
- [Agent Skills specification](https://agentskills.io/specification): complete skill packages and metadata.
- [MCP protocol revision migration](https://ts.sdk.modelcontextprotocol.io/v2/migration/support-2026-07-28): why the bridge must validate version-specific lifecycle behavior.

The release architecture, command syntax, dependency choices, group rules, and milestones are recommendations, not claims that these capabilities already exist.
