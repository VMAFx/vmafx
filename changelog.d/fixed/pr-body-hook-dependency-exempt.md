- **The pre-push PR-body check skips a dependency-only bot PR, as CI's
  deliverables checklist does (ADR-1152).** `scripts/git-hooks/pre-push-pr-body-lint.sh`
  exempted only the release PR, so a push of a Renovate PR that changes only
  `go.mod` and `go.sum` failed with six missing ADR-0108 deliverables while CI exempts it.
  The hook now asks `scripts/ci/classify-dependency-pr.sh` with the author, head ref and
  changed files CI passes it. A human PR or a bot PR that touches source is still validated,
  and the public-page fallback never exempts.
