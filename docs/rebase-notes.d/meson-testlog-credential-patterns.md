## Meson testlog credential patterns (2026-11-05)

- `scripts/ci/run_meson_test.py`: `CREDENTIAL_NAME_PATTERN` and
  `is_credential_name()` widen the ADR-1333 scrub to credential-shaped
  names (ADR-3400). Fork-only file; nothing upstream to merge.
- `scripts/ci/check_testlog_credentials.py` (new) and the
  `testlog-credential-scan` hook in `.pre-commit-config.yaml`.
  **On sync**: none of these files exist upstream; keep the hook when
  resolving `.pre-commit-config.yaml` conflicts.
