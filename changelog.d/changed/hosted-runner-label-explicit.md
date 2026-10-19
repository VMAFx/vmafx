- **Every hosted Linux CI job names its image by version (ADR-3251).** GitHub moves the
  `ubuntu-latest` label from Ubuntu 24.04 to 26.04 in a rollout from 2026-10-19 to 2026-11-19. The
  63 jobs and the one runner-pool fallback that still named the floating label now name
  `ubuntu-26.04`, as the other hosted Linux jobs already did, and the four licence-companion jobs
  move from `ubuntu-24.04` to it. `verify-native-artifacts` stays on `ubuntu-24.04` on purpose: it
  proves that the native Linux bundle loads on the oldest hosted image (ADR-1354). A new check,
  `scripts/ci/check_hosted_runner_labels.py` (pre-commit hook `check-hosted-runner-labels`, third
  step of `make lint-actions`), refuses a floating label and an older image without a declaration.
  A job that needs an older image for a while is declared in
  `.config/lint-exceptions.d/hosted-runner-label.toml` with an end date at most 90 days out.
  ADR-3251 supersedes ADR-0802. No score, symbol, option or published artefact changes.
