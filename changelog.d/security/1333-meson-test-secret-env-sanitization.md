- **Meson test environment secret credential sanitization (ADR-1333)** —
  configured a default test setup in `core/meson.build` using `environment().unset()`
  to purge secret-bearing GitHub credentials (`GITHUB_PERSONAL_ACCESS_TOKEN`,
  `GITHUB_TOKEN`, `GH_TOKEN`, `GH_ENTERPRISE_TOKEN`, `GITHUB_ENTERPRISE_TOKEN`, `GITHUB_PAT`,
  `GH_PAT`, `GITHUB_AUTH_TOKEN`, `GITHUB_API_TOKEN`, `HOMEBREW_GITHUB_API_TOKEN`) from test process
  execution environments and `build/meson-logs/testlog.json`. Non-secret and ordinary
  runtime environment variables (`PATH`, `HOME`, `GITHUB_ACTIONS`, `GITHUB_REPOSITORY`)
  are preserved, preventing credential leakage in test runs, crash reports, and CI log
  artifacts without weakening test coverage. Accompanied by a red-capable regression
  contract in `core/test/test_meson_secret_env_sanitization.py`.
