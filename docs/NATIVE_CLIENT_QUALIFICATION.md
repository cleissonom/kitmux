# Native client qualification

Qualified on 11 November 2026, macOS arm64, using the compiled Kitmux 0.3.0
binary and the installed clients below. These are actual client processes, not
replacement implementations of their MCP clients.

| Client | Installed version | Qualified surface |
|---|---|---|
| Codex | `codex-cli 0.167.1` | Stdio app-server, one ephemeral thread, native MCP status and tool-call APIs |
| Claude Code | `2.1.322` | One persistent `--bare --print` process with stream JSON input/output and an explicit strict MCP configuration |
| OpenCode | `v2.1.4` | Private loopback HTTP server, one `build` session, native Code Mode tool execution |

## Reproduce

Run from the repository root with those clients installed on `PATH`:

```sh
make test-native
```

Equivalent command after building:

```sh
KITMUX_NATIVE_TESTS=1 KITMUX_BINARY="$PWD/build/kitmux" PYTHONPATH="$PWD/tests" \
  python3 -m unittest test_client_compatibility test_subagent_native_codex \
    test_subagent_native_claude test_subagent_native_opencode -v
```

The target now includes three MCP lifecycle cases and three custom profile cases.
Ordinary `make test` skips these opt-in tests so it remains usable on machines
without all three clients installed. Final v0.3 results are recorded in
[architecture and verification](IMPLEMENTATION.md).

The harness uses Python's standard library. Python is not a Kitmux runtime
dependency; it supplies the test MCP and local protocol fixtures only.

## Isolation

Each case creates and removes a temporary home, native client configuration,
working directory, XDG directories, and Kitmux catalog/state. An allowlisted
subprocess environment excludes inherited provider credentials and session
tokens. Client and bridge processes launched by the harness are closed during
cleanup. OpenCode traffic uses the fixture-owned server only.

Codex runs MCP-only app-server methods without a model turn or provider key.
Claude uses `--bare`, which the installed client's help explicitly documents as
skipping OAuth/keychain reads, and loads only the generated strict MCP fixture.
Its Anthropic endpoint is a loopback HTTP fixture with a synthetic test key.
The Claude profile test needs automatic discovery, which `--bare` suppresses.
It instead runs the native loader under macOS `sandbox-exec`, denying live home
and keychain reads, Mach lookups, the security executable, and outbound traffic
except its synthetic provider's loopback port. This case skips on other platforms.

OpenCode likewise uses an isolated custom OpenAI-compatible provider on loopback
and a synthetic password for its private HTTP server. No real account, remote
model, production MCP, or live agent configuration participates.

The provider fixtures do not perform inference. They return deterministic tool
requests and completion messages using the clients' supported wire formats.
Any cost estimates printed by those native clients are calculations for the
synthetic responses, not billed provider requests.

## Covered lifecycle

The three MCP lifecycle cases use `kitmux sync --apply --agent <client>` against
a temporary home and exercise a local managed stdio MCP through its native entry.

1. The actual client initializes the bridge and discovers the fixture tool.
2. A native tool invocation reaches the upstream fixture.
3. Kitmux disables that resource for the client while the client remains running.
4. A request using the previously known tool must not reach the upstream.
5. Kitmux re-enables it and a native invocation succeeds in the same client
   connection/session.
6. `kitmux mcp reconnect` replaces the owned upstream process; another native
   invocation succeeds and observes a different upstream PID.

Codex calls `mcpServerStatus/list` and `mcpServer/tool/call` on the same app-server
thread. Opaque caller `_meta` keys and Codex's added `threadId` are checked before
and after reconnect.

Claude's local fixture emits a request for `mcp__stub__identity`; the real Claude
CLI executes it. The disabled call yields a native tool error and no upstream
call. After enabling, the test polls native `mcp_status` until the tool returns
before continuing. This qualifies the persistent print/SDK surface, not an
interactive terminal or desktop client. It does not infer an undocumented
Claude conversation ID in MCP metadata.

OpenCode's local fixture emits its native `execute` tool with the known
read-only route `tools.stub.identity`. The session remains the same throughout.
The upstream log checks that `_meta["ai.opencode/sessionID"]` equals that native
session's ID before and after reconnect. The disabled cached route produces no
upstream call.

## Asynchronous native discovery

A connected native MCP transport does not prove that its model-facing tool
registry has finished updating. OpenCode v2.1.4 exhibited this both with Kitmux
and with the fixture connected directly. Its versioned source separately
registers initial tools and debounces subsequent tool-change events by 100 ms.
See [OpenCode v2.1.4 MCP tool registration](https://github.com/anomalyco/opencode/blob/v2.1.4/packages/core/src/tool/mcp.ts).

The qualification test therefore retries completed **read-only fixture** turns
with a bounded deadline and a short poll cadence until exactly one invocation
has reached the fixture. Unavailable-tool attempts have no upstream effect;
the test stops retrying as soon as a call is observed. This is a readiness probe
inside the test harness. Kitmux itself never replays an interrupted MCP call.
Claude's test instead waits on its native catalog status.

The OpenCode harness also correlates completion to the prompt's native inbox ID
and a unique fixture phase marker in a completed assistant message. An absent
entry in the active-session map alone is insufficient: queued turns can briefly
appear inactive before execution. This prevents overlapping readiness attempts
and preserves the exact one-call-per-successful-phase assertions.

The initial qualification failures exposed this registry timing distinction;
the harness was corrected to observe readiness. No production check or runtime
guard was weakened to obtain the native passes.

## Custom subagent discovery

V0.3 adds three cases using definitions generated and controlled by Kitmux:

- Codex starts fresh ephemeral app-server sessions with a loopback Responses
  fixture. The actual model request context includes the enabled profile's unique
  description, excludes it after disable, and includes it after re-enable. The
  synthetic response completes each turn without contacting OpenAI.
- Claude starts fresh print sessions with `--agent` selecting the generated
  profile. Native initialization lists it and the fixture receives its unique
  system instructions. Disable makes selection fail before any provider request;
  re-enable restores selection. OS restrictions preserve account isolation.
- OpenCode exposes both a generated Markdown profile and a native JSON profile
  through its location-scoped `/api/agent` list. Each disappears on disable and
  returns on re-enable in the same private server. Polling waits for configuration
  readiness because the first list can be empty even after OpenAPI is available.

These cases qualify native discovery and selection. They do not execute delegated
tasks or imply that disabling a definition stops an already-running subagent.

## Limits

- Qualification covers these exact versions and local stdio tools using the
  supported legacy MCP initialization lifecycle. It does not qualify the modern
  stateless MCP lifecycle, every server capability, remote HTTP/OAuth, or all
  future client versions.
- Codex's desktop/IDE clients, Claude's interactive UI, OpenCode's shared service,
  custom agent permission enforcement, and organization/project policy combinations were
  not exercised by these cases.
- Native tool-list changes are asynchronous. A successful reconnect establishes
  upstream readiness; it does not certify that every native UI or model catalog
  has already refreshed.
- User configuration is the adapter scope. Native project/organization overrides
  still apply. Sharing definitions does not merge clients' accounts or sessions.
- Session-bound integrations such as ai-memory retain their direct-mode default;
  this synthetic fixture does not certify their application-level identity rules.

Relevant primary documentation:

- [Codex app-server](https://learn.chatgpt.com/docs/app-server)
- [Claude CLI reference](https://code.claude.com/docs/en/cli-reference)
- [OpenCode MCP configuration, Code Mode, and session metadata](https://opencode.ai/docs/mcp-servers/)
- [OpenCode custom providers](https://opencode.ai/docs/providers/)

Native CLI help and Codex's generated app-server schema were also inspected in
isolated subprocesses. OpenCode's actual private `/openapi.json` supplied its
HTTP request schemas; previous-version server documentation was not used as the
v2 API contract.
