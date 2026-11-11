# Security reporting

Use the repository's **Security → Report a vulnerability** option for private
reports when GitHub private vulnerability reporting is enabled. If it is not
available, open an issue asking for a private reporting channel without including
the vulnerability details or sensitive data.

Include the Kitmux version, platform, affected client/version, impact, and a
minimal reproduction using a temporary home and synthetic configuration. Omit
tokens, private server addresses, real catalog/journal files, and account data.
Reports and fixes are handled on a best-effort basis for the latest release.

Kitmux manages native configuration and can launch registered MCP commands.
Only register definitions and install skill packages you trust. Group assignment
controls availability; native clients continue to enforce permissions and own
their sessions. See the [configuration scope and recovery guidance](README.md#configuration-safety-and-scope).
