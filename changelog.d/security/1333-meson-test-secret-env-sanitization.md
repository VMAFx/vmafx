- **Meson test environment secret credential sanitization (ADR-1333)** —
  added `scripts/ci/run_meson_test.py` and routed every supported Make, CI, preflight,
  bisection, setup-guidance, and Zed test entry point through it. The wrapper deletes
  secret-bearing GitHub credentials (`GITHUB_PERSONAL_ACCESS_TOKEN`,
  `GITHUB_TOKEN`, `GH_TOKEN`, `GH_ENTERPRISE_TOKEN`, `GITHUB_ENTERPRISE_TOKEN`, `GITHUB_PAT`,
  `GH_PAT`, `GITHUB_AUTH_TOKEN`, `GITHUB_API_TOKEN`, `HOMEBREW_GITHUB_API_TOKEN`,
  `ACTIONS_ID_TOKEN_REQUEST_TOKEN`, `ACTIONS_RUNTIME_TOKEN`) before Meson can record its
  parent environment in `testlog.txt`. The default test setup remains a second defense for
  child environments and `testlog.json`. Fail-closed contracts reject raw supported-entry
  point bypasses across shell, multiline YAML (plain and quoted keys), and Python implicit
  list/tuple continuations, alternate test setups, and explicit per-test credential
  reintroduction. Subprocess probes default to a load-tolerant 120-second deadline, and the
  optional override fails closed unless it is finite and within 60--300 seconds.
  Disposable RED/GREEN probes cover both log formats using synthetic values only. Direct raw
  Meson or Ninja test-target commands remain a documented unsupported bypass.
