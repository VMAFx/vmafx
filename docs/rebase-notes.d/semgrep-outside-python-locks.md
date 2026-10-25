## Semgrep outside the Python locks (2026-10-25)

Fork-only. `requirements/locks/tooling-tests.in` must not regain `semgrep`; the vendored-scope
test reads the Semgrep image from `.github/workflows/security-scans.yml` and needs exactly one
digest-pinned `semgrep/semgrep` image line there. The `mcp` entry of
`.config/lint-exceptions.d/dependency-advisory.toml` and the GHSA-84m7 block of
`osv-scanner.toml` are gone; an upstream sync has nothing to merge here
([ADR-3375](../adr/3375-semgrep-outside-python-locks.md)).
