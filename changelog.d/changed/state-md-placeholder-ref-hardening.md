- **`state-md-touch-check` CI gate hardened against placeholder PR/commit
  refs ([ADR-0334](../docs/adr/0334-state-md-touch-check-ci-gate.md)
  status update 2026-05-09).** PR #541's comprehensive `docs/state.md`
  row audit ([Research-0090](../docs/research/0090-state-md-row-audit-2026-05-09.md))
  surfaced a drift mode the original 2026-05-08 gate did not catch:
  closing PRs were writing the literal `this PR` as the closer-PR
  placeholder, the merge happened, and the placeholder never got
  rewritten to the merged numeric PR number. 8 of 41 audited
  "Recently closed" sub-rows were stale for this exact reason.
  [`scripts/ci/state-md-touch-check.sh`](../scripts/ci/state-md-touch-check.sh)
  now additionally REJECTS inserted lines in `docs/state.md` that
  contain any of: `this PR`, `this commit`, bare `TBD`, the literal
  `<PR>` template placeholder, or `#NNN` (real refs use digits).
  Canonical accept forms are `PR #N` and ``commit `<sha>` ``. Failure
  message points at the offending lines and names the canonical
  replacement. The hardening is additive — every existing pass / fail
  case from the original gate remains unchanged (per the fork's
  `feedback_no_test_weakening` rule). Companion fixture script gains
  10 cases (8 reject + 2 accept) covering each placeholder form plus
  two regressions: removed-line placeholders are NOT flagged, and
  substrings like `debug-pr` (no whitespace between `this` and `pr`)
  do not false-match. Total fixture-script cases: 18.
