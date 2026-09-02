## Added

- **Codecov upload from both Coverage Gate jobs**
  (`.github/workflows/tests-and-quality-gates.yml`): the existing
  gcovr-produced Cobertura XML (`core/build-coverage/coverage.xml` for
  the CPU gate, `core/build-coverage-gpu/coverage.xml` for the
  self-hosted GPU gate) is now uploaded to Codecov via
  `codecov/codecov-action@v6.0.1` (SHA-pinned to
  `cddd853df119a48c5be31a973f8cd97e12e35e16`) with fork-aware OIDC
  (`use_oidc: true`, no `CODECOV_TOKEN` secret required). Uploads are
  flag-tagged `cpu` vs `gpu` so the Codecov UI separates the two
  coverage flavours. `fail_ci_if_error: false` — the in-tree gcovr
  threshold gate (ADR-0114 / ADR-0117 / ADR-0637) remains the only
  blocking check; Codecov upload is informational. Closes the gap PR
  #383 documented. See ADR-0903.
