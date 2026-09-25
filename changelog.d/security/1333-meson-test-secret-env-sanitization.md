- **Meson test environment secret credential sanitization (ADR-1333)** —
  configured a default test setup in `core/meson.build` using `environment().unset()`
  to purge secret-bearing GitHub credentials (`GITHUB_PERSONAL_ACCESS_TOKEN`,
  `GITHUB_TOKEN`, `GH_TOKEN`, `GH_ENTERPRISE_TOKEN`, `GITHUB_ENTERPRISE_TOKEN`, `GITHUB_PAT`,
  `GH_PAT`, `GITHUB_AUTH_TOKEN`, `GITHUB_API_TOKEN`, `HOMEBREW_GITHUB_API_TOKEN`,
  `ACTIONS_ID_TOKEN_REQUEST_TOKEN`, `ACTIONS_RUNTIME_TOKEN`) from the child environments
  and `build/meson-logs/testlog.json` of every currently declared test. A fail-closed
  contract rejects alternate test setups and explicit per-test credential reintroduction;
  its hermetic probes use only an allowlisted platform environment plus synthetic values.
