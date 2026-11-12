# Kitmux engineering

Kitmux is a C17 terminal application. Keep production behavior in C; Python is
used only for isolated subprocess/PTY tests. Prefer existing helpers, focused
functions, and small changes that preserve public contracts and persisted data.

For behavior changes, add a failing regression test before implementation, then
run the relevant tests and required checks. Documentation and test-harness work
do not need a manufactured failing test. Keep this repository self-contained;
contributors do not need any machine-specific agent skills or configuration.

Build with `make`; run `make test` and `make sanitize`. Dependencies come from
pkg-config: json-c, yaml-0.1, ncursesw, and libcrypto. Never install dependencies
or mutate live client configuration as part of a test.

CI targets macOS 15 and Ubuntu 22.04, each on arm64 and x86_64. Release builds use
`make release VERSION=vX.Y.Z`, pinned source archives, and private dependency
prefixes; never install their bundled libraries globally. Keep native tests
opt-in, and check packaged binaries without development libraries before release.
The README's marked installation block is exercised directly by subprocess tests;
keep its staged download/checksum/install behavior safe on both supported systems.

`make test-native` opts into installed-client qualification. Keep its client homes,
authentication, provider responses, and MCPs isolated; use synthetic loopback
providers and never load live accounts. Normal tests skip these probes.
The target covers both managed MCP lifecycle and custom subagent discovery/off/on.
Claude profile discovery uses the macOS sandbox wrapper in its native test because
`--bare` suppresses discovery; preserve its account and network isolation.
`make test` also builds the C shutdown fixture against production runtime objects.

Use `--home <temporary-directory>` for integration tests. Native client configs,
skill links, and MCP processes outside that root must remain untouched. Preserve
unowned configuration and reject conflicts. Never print imported secrets.

Keep files focused and below 500 lines where practical. Put tests before behavior
changes. Record implementation progress and limitations in docs/IMPLEMENTATION.md.
