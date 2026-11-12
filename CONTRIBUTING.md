# Contributing to Kitmux

Bug reports, documentation improvements, and focused pull requests are welcome.
For larger features or changes to native configuration behavior, open an issue
first so the design and compatibility requirements can be discussed.

## Local development

Install the dependencies in the [README](README.md#build-and-run-from-source), including
Python 3.11+ for tests, then run:

```sh
make
make test
make sanitize
```

Production code is C17. Tests use Python's standard library and local fixtures;
the application does not require a Python or Node runtime. `make test-native`
is an optional qualification suite for installed coding clients, with isolated
homes and synthetic providers. See [native qualification](docs/NATIVE_CLIENT_QUALIFICATION.md).
GitHub Actions runs the ordinary and sanitizer suites on macOS 15 (arm64 and
x86_64) and Ubuntu 22.04 (arm64 and x86_64). Exact qualification evidence and
remaining limitations are recorded in [the implementation notes](docs/IMPLEMENTATION.md).

## Release builds

`make release VERSION=v0.5.0` builds pinned dependency archives in a private
`build-release-deps` prefix and packages the executable under `dist/`. It needs
CMake, Perl, curl, and the normal compiler/build tools. The build downloads and
checks source archives; it never installs those dependencies into the host.
The executable retains only platform system libraries. Linux builds must run
on glibc 2.35 (Ubuntu 22.04) to meet the supported baseline; macOS builds target
15.0. System terminfo data is required for the TUI.

The release workflow runs all tests, checks dependency linkage and architecture,
and smoke-tests packaged executables without development libraries. Manual runs
produce downloadable artifacts and checksums. A matching version tag publishes
only after all four targets pass; publication stages a draft before exposing the
complete asset set. Normal tests use staged local download fixtures and never
download or build release dependencies.

## Making a change

1. Keep the change focused and follow the surrounding C code and existing helpers.
2. For behavior changes, add a regression test, confirm it fails, implement the
   fix, and rerun the relevant tests. Documentation-only changes need a content
   and link check rather than a manufactured failing test.
3. Run `make test` and `make sanitize` before submitting code changes. Explain any
   check you could not run and include the exact failure or limitation.
4. Update the README or relevant documentation when commands, native formats,
   persistence, or user-visible behavior change.

Tests must use temporary homes through `--home DIR`. Never use live client
configuration, personal accounts, provider credentials, or production MCP servers
as test fixtures. Native adapters must preserve unowned values, reject conflicts,
and retain restore behavior. Do not print imported credentials in diagnostics.
Keep vendored code changes separate and retain its upstream notices.

## Issues and pull requests

For bugs, include Kitmux/client versions, OS/architecture, minimal reproduction
steps, expected behavior, and the observed error. Use synthetic configuration and
redact private paths and values. Do not attach a real catalog, ownership journal,
authentication file, or complete client configuration.

For pull requests, describe the problem, resulting behavior, and verification.
Include the failing regression result when relevant. Small, reviewable changes
are easier to assess than unrelated cleanup bundled with a feature.

Discuss the code respectfully and explain disagreements with evidence. Use
[private security reporting](SECURITY.md) for vulnerabilities.

Contributions are provided under the project's [MIT license](LICENSE). Retain
appropriate attribution and third-party notices for any code you introduce.
