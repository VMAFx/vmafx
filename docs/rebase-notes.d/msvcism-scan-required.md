## msvcism scan: no reset fallback, required in Pre-Commit (2026-10-18)

- `scripts/dev/preflight.sh`, stage `msvcism`: every per-file scan ends its
  grep pipeline with `|| [ $? -eq 1 ] || exit 2`, and the scan's assignment is
  followed by `|| scan_failed '<name>'`. **On sync or rebase**: do not bring
  back a `|| hits=""` style fallback after a `while read` scan; it erases the
  findings whenever the last file is clean (ADR-3351). The static-aggregate
  scanner is `$PREFLIGHT_DIR/find-nonconst-static-init.py`, not under
  `$REPO_ROOT`.
- `.github/workflows/lint-and-format.yml`, job `pre-commit`: keep the step
  "MSVC-hostile constructs in the changed files".
- `scripts/ci/tests/test-preflight-msvcism.sh` cases "a finding is still
  reported when a clean file is scanned after it" and "a static aggregate
  initialised from a const variable fails the stage" fail against the old
  script; keep them.
