---
paths:
  - scripts/ci/setup-envtest.sh
  - scripts/ci/tests/test_envtest_single_source.py
invariant: `setup-envtest.sh` consumes the `build-config.env` envtest fields; only `install` fetches; no `@latest`.
area: tests
---
<!-- markdownlint-disable MD013 MD060 -->
# Shared envtest installer (ADR-1231)

`setup-envtest.sh` = executable consumer of envtest tool/version
fields in `build-config.env`. Both Make and Go CI call it; keep Go module
metadata check, direct GOBIN/first-GOPATH executable path, and installed-only
`path`/`env` lookup. Only `install` may fetch assets; inherited
`ENVTEST_USE_ENV` must not bypass configured selection. Do not restore
`@latest`, PATH-existence acceptance or second Kubernetes default in CI.
Preserve install/asset failures and shell-quoted export output; keep
`tests/test_envtest_single_source.py` wired to commit/push checks. See
[Research-2058](../../../docs/research/2058-envtest-version-owner.md).
