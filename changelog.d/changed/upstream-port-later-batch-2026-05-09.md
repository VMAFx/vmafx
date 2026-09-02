- Closed out the Research-0090 PORT_LATER bucket (18 upstream SHAs)
  with explicit per-commit verdicts and reopen triggers in
  [`docs/state.md`](docs/state.md) + [`docs/rebase-notes.md`](docs/rebase-notes.md).
  All 18 commits remain DEFERRED — 17 are subsumed by the in-flight
  PR #497 (`chore/upstream-port-mytestcase-migration-v2-2026-05-08`,
  +7372/-652) and the eighteenth (`721569bc`, cambi docs) is already
  duplicate-covered by PR #443 and PR #444. Two pure-deletion commits
  (`25ff9f18` empty `VmafossexecCommandLineTest` stub; `0341f730`
  duplicate `test_run_vmaf_integer_fextractor`) are flagged as
  cherry-pick-after-#497 follow-ups because PR #497's diff state
  currently re-emits both identifiers. Netflix-golden guard reaffirmed:
  the four upstream macOS-FP tolerance commits (`4679db83`,
  `ead2d12b`, `6c097fc4`, `d93495f5`) explicitly LOWER `places=` on
  a subset of golden assertions and PR #497 must preserve fork
  tolerances byte-for-byte on the three Netflix CPU golden pairs per
  CLAUDE §8 / ADR-0024. No code touched in this PR; no rebase impact
  beyond the documentation entries themselves.
