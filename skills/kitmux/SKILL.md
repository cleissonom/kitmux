---
name: kitmux
description: Manage MCP servers, Agent Skills, custom subagents, groups, and client assignments through the Kitmux CLI for Codex, Claude Code, and OpenCode. Use for availability changes, imports, reconnects, skill updates, subagent definitions, and native configuration sync or restore.
---

# Kitmux

Use Kitmux to manage resources it owns. Perform the user's requested changes
within their existing authorization; a preview is a review aid, not a requirement
to ask for permission again.

## Locate the CLI

Run `command -v kitmux`, then `kitmux --version` and `kitmux --help` as needed.
If it is absent and a Kitmux checkout is available, run `make` in that checkout
and use `./build/kitmux` there, or its absolute executable path. Installed help is
the command contract. Use `kitmux --home DIR ...` only for an explicitly selected
alternate home or an isolated test.

Start with `kitmux status --json` and the relevant `group list`, `mcp show ID`, or
`skill list`. In CLI flags, `--agent` means a **client**: `codex`, `claude`, or
`opencode`.

## Adopt and synchronize

Preview the selected clients before initial adoption:

```sh
kitmux import --from codex,claude,opencode --preview
kitmux import --from codex,claude,opencode --apply
kitmux sync --preview --agent codex
kitmux sync --apply --agent codex
```

Inspect what this version's import actually reports; do not claim skipped
resources were adopted. Preserve client-specific definitions and authentication
when resolving conflicts. Never replace one client's native settings with
another client's settings just to make import succeed.

Import supports per-client MCP execution variants and existing filesystem skill
aliases. Bundled system/plugin skills are not ordinary user packages. Preserve
native disable rules; a blocked native skill may need its specific client override
changed before it can be enabled through Kitmux.

After adoption, catalog mutations automatically sync the adopted clients.
If an error says **catalog saved; native sync pending**, retain the desired
catalog change, resolve the specific native conflict, and retry `sync --preview`
then `sync --apply`. Do not force ownership by editing its private journal.

## Groups and availability

Groups can mix MCPs, skills, and subagents. Use existing IDs from the catalog:

```sh
kitmux group add communication
kitmux group include communication mcp:teams skill:meeting-notes
kitmux group assign communication --agents codex,claude,opencode
kitmux group enable communication
kitmux mcp inherit teams --agent codex
kitmux skill disable meeting-notes --agent opencode
```

- Global resource disable overrides every assignment.
- Explicit per-client `disable` overrides groups. Per-client `enable` is a direct
  assignment; `inherit` returns that client to group control.
- Otherwise, any enabled group assigned to the client can enable its resources.
  Turning off one group may leave a resource available through another group.
- Imported direct assignments can keep a resource on after a group is disabled;
  inspect its reason and use `inherit` when group control is intended.
- An unassigned resource is unavailable. Group assignment replaces the group's
  client list; `--agents ''` assigns none.

Use `group exclude GROUP mcp:ID|skill:ID`, `group disable GROUP`, or resource
`remove ID` for the requested scope. A group change does not imply deletion of
its packages or native credentials.

## MCP definitions and runtime

```sh
kitmux mcp add local-search --command /path/to/server -- --stdio
kitmux mcp add docs --url https://example.invalid/mcp
kitmux mcp edit local-search --mode managed
kitmux mcp status local-search --agent codex --json
kitmux mcp reconnect local-search --agent codex --json
```

Adding a definition does not install its executable. Keep imported MCPs in direct
mode unless managed mode is explicitly intended and compatible.

**Direct:** the native client owns the connection, including HTTP/OAuth.
Reconnect may return `action_required`; report its native reload/reconnect step,
not a successful reconnect. Preserve session-bound integrations such as ai-memory
in direct mode.

**Managed:** Kitmux controls a session-independent local stdio upstream through
its bridge. A disabled upstream can leave the bridge registered. Reconnect
replaces owned upstreams and never replays an interrupted call. Completion of an
interrupted side effect can be unknown; do not automatically invoke it again.

Distinguish desired availability, observed processes, and native tool refresh.
`running` is process evidence; `tool_refresh: unverified` is not proof that the
client has refreshed. Native project/organization policy still applies.

## Skill packages

```sh
kitmux skill add /path/to/skill-directory
kitmux skill add owner/repository --path skills/reviewer --ref COMMIT
kitmux skill enable reviewer --agent claude
kitmux skill update reviewer --preview
kitmux skill update reviewer --apply
kitmux skill rollback reviewer
```

Use the installed skill's declared ID, not an assumed repository name. New
packages start unassigned; enable them for selected clients or include them in an
assigned group. Review revision/content changes before applying an update.
Disabling or removing a skill affects future discovery; instructions already in
a conversation remain until that context is replaced. Shared native discovery
paths can cause conflicts—resolve those rather than claiming isolation.

## Diagnose and restore

Use `kitmux doctor`, `status --json`, and targeted MCP status. For requested
rollback, run `restore --preview --agent CLIENT`, then `restore --apply --agent
CLIENT`. Successful restore releases that client from automatic management;
explicit import/sync can adopt it again.

The catalog is bounded to 8 MiB. Catalogs, ownership journals, and native config
can contain secrets; do not dump them into chat or logs, copy credentials between
clients, or bypass limits with manual rewrites. Prefer sanitized CLI summaries
and inspect only the specific fields needed, with sensitive values redacted.

## Custom subagents (v0.3)

Subagents are reusable native profiles; `--agent` still names the coding client.
Use `subagent import` to adopt only profiles; ordinary `import` includes MCPs,
skills, and profiles. Import previews preserve native files and omit their bodies.

```sh
kitmux subagent import --from codex,claude,opencode --preview
kitmux subagent import --from codex,claude,opencode --apply
kitmux subagent add code-reviewer --from ./review.md --description "Review code changes"
kitmux group include engineering subagent:code-reviewer
kitmux subagent enable code-reviewer --agent codex
kitmux subagent disable code-reviewer --agent claude
kitmux subagent edit code-reviewer --from ./review-updated.md
kitmux subagent duplicate code-reviewer release-reviewer
```

Plain Markdown plus a description creates portable instructions with native
defaults. Native source files require `--format codex|claude|opencode` (or
`opencode-json` for a single JSON definition). A native-format edit adds or
replaces that client's variant. Imported profiles retain native settings and need
an explicit variant before sharing to a different client; do not convert away
model, tool, or permission restrictions to force availability.
Keep an adopted/projected OpenCode profile in its current Markdown or JSON format.
V0.3 manages flat, regular-file collections; nested definitions and symlinked
collections need explicit migration before adoption.

New and duplicated profiles begin unassigned. Shared instruction edits preserve
native options. Inspect `subagent list`, `subagent show ID`, and `status --json`
for compatibility and dependency information. Missing catalog dependencies can
still be supplied natively; do not silently enable or install them.

The Subagents tab is `4`: `a` assigns clients, Space toggles a checkbox, Enter
saves, and `e` controls global availability. Changes affect user-level discovery
after native refresh. Project/plugin definitions may override them, and running
subagents retain their context. Kitmux does not execute, stop, or reconnect custom
subagents. `restore` recovers owned native definitions and releases management.
