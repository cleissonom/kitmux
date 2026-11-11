# Managed runtime compatibility

Kitmux's managed stdio bridge implements the legacy MCP initialization lifecycle
and the tools capability. It does not implement the 2026 stateless discovery,
subscription, or task lifecycle. Direct mode remains available for servers that
need resources, prompts, roots, sampling, elicitation, or other unsupported features.

## Protocol behavior tested locally

The subprocess tests in `tests/test_runtime.py` use a local Python fixture, temporary
homes, and the compiled C executable. They verify:

- A pre-initialization `server/discover` receives JSON-RPC `-32601` without starting
  an upstream process. A client can then initialize on the same connection.
- Legacy initialization retains a supported offered version. An unsupported offer,
  including a date in the modern era, receives `2025-11-25`; the retained upstream
  initialization uses that negotiated version.
- Stateless tool requests without legacy initialization are rejected.
- Disabled catalogs, metadata forwarding, reconnect, interrupted calls without
  replay, process-group cleanup, and isolation from unrelated agent policy edits.
- Supported upstream-selected legacy revisions, scoped cancellation, correlated
  progress, and read-only process diagnostics across independent connections.

The bridge recognizes legacy versions `2024-11-05`, `2025-03-26`, `2025-06-18`, and
`2025-11-25`. Its focused negotiation fixtures exercise `2025-06-18`, `2025-11-25`,
and an unsupported `2026-07-28` offer. Upstream initialization accepts any supported
legacy revision; unknown revisions and unsupported capabilities produce an
actionable direct-mode error. Downstream and upstream negotiated versions are
separate observations.

## Client source and documentation evidence

These are source/documentation findings. Actual isolated client tests are recorded
separately in [native client qualification](NATIVE_CLIENT_QUALIFICATION.md); they
do not use live accounts or certify arbitrary client/server combinations.

- **Codex 0.167.1:** the tagged `McpProtocolMode` source selects `2025-06-18` for
  legacy mode and retains that version as fallback in automatic discovery modes.
  Actual session mode can depend on feature selection. The pinned Rust SDK
  classifies a correlated non-modern discovery error as a legacy peer.
  [Codex tagged protocol policy](https://github.com/openai/codex/blob/rust-v0.167.1/codex-rs/rmcp-client/src/protocol_mode.rs),
  [pinned Rust SDK client](https://github.com/modelcontextprotocol/rust-sdk/blob/98a54789b80110ad210495c4ad845c89e1980471/crates/rmcp/src/service/client.rs).
- **Claude Code 2.1.322 environment:** current documentation describes the v2
  runtime probing modern support and using the earlier handshake for legacy peers.
  `MCP_PROTOCOL_NEGOTIATION=legacy` is an optional setting on the Claude client
  process, rather than on the child MCP process.
  [Claude MCP runtimes](https://code.claude.com/docs/en/mcp#mcp-client-runtimes).
- **OpenCode v2 documentation:** `protocol: "legacy"` is the default; `auto` probes
  and falls back, while `2026-07-28` requires modern support. Installed-client
  versions are recorded separately in the native qualification results.
  [OpenCode protocol selection](https://opencode.ai/docs/mcp-servers/#protocol-version).

The [MCP legacy lifecycle specification](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle#version-negotiation)
requires the server to offer a version it supports when it cannot accept the
client's offered version. Returning a supported legacy date from `initialize`
does not claim support for the modern lifecycle. The
[official TypeScript SDK migration guide](https://ts.sdk.modelcontextprotocol.io/v2/migration/support-2026-07-28)
documents discovery probes and automatic legacy fallback.

## Remaining qualification

Rerun `make test-native` when a supported client release changes. Qualification
applies to its recorded client surface and local read-only fixture. Keep live
accounts and existing MCP definitions outside the fixture configuration. Project
and organization settings, GUI sessions, authenticated servers, and unsupported
capabilities require their own qualification. Verify tool refresh after
disable/re-enable and reconnect separately from initial discovery; a successful
upstream reconnect alone is not evidence of client refresh.
