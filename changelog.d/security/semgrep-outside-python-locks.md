- **Semgrep no longer comes from a Python lock.** `semgrep` pinned `mcp` 1.29.0 and
  `opentelemetry-api` 1.37 exactly, so their advisories stayed open against
  `requirements/locks/tooling-tests.txt`. The vendored-scope test now runs Semgrep from the
  pre-commit hook's environment or the digest-pinned image of the `Semgrep` CI job, both
  packages leave the lock, and the declared `mcp` exception and its osv-scanner ignore are
  removed ([ADR-3375](../docs/adr/3375-semgrep-outside-python-locks.md)).
