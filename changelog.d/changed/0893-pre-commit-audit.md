- **chore(pre-commit):** Audit `.pre-commit-config.yaml` — add
  `forbid-new-submodules` (the fork pulls upstream via Meson wraps and
  `ffmpeg-patches/`, never via `.gitmodules`; this guards against
  accidental submodule additions bypassing the wrap-pin machinery);
  bump `isort` 5.13.2 → 6.0.1; bump `ruff-pre-commit` v0.15.13 →
  v0.15.15. Keep `gitleaks` at v8.30.1 (autoupdate suggested an
  out-of-order downgrade to v8.30.0; rejected after verifying upstream
  tag order). Does not touch the `semgrep-local` block (PR #340) or
  insert a `markdownlint-cli2` block (PR #342). (ADR-0893)
