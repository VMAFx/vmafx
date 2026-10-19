## Hosted Linux jobs name ubuntu-26.04 explicitly (2026-10-11)

`ci/hosted-runner-label-2604`, [ADR-3251](adr/3251-hosted-runner-label-explicit.md). A rebase or
sync keeps `ubuntu-26.04` / `ubuntu-26.04-arm` on every hosted Linux job of `.github/workflows/`. A
branch that adds a job, a matrix value or an expression fallback with `ubuntu-latest`,
`ubuntu-slim` or an older versioned image fails `scripts/ci/check_hosted_runner_labels.py`
(pre-commit hook `check-hosted-runner-labels`, third step of `make lint-actions`): take the
explicit label when resolving the conflict, never the floating one. `verify-native-artifacts` in
`supply-chain.yml` keeps `ubuntu-24.04` (ADR-1354); the script lists it in `COMPATIBILITY_TARGETS`
and fails when the job stops naming that image. A job that fails on `ubuntu-26.04` goes back to an
older label only together with an entry in `.config/lint-exceptions.d/hosted-runner-label.toml`
(workflow file, job, label, reason, end date at most 90 days out); the file does not exist while no
job needs one. Comments in install steps that describe the 24.04 image were left as they are.
