# Contributing to Kitmux

Bug reports, documentation improvements, and focused pull requests are welcome.
For larger features or changes to native configuration behavior, open an issue
first so the design and compatibility requirements can be discussed.

## Local development

Install the dependencies in the [README](README.md#build-and-run), including
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
macOS arm64 is the verified platform; reports and fixes for Linux are welcome.
GitHub Actions runs the ordinary and sanitizer suites on macOS.

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
