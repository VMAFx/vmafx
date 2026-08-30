- **OSSF Scorecard workflow restored to green (ADR-0263)** — the
  `scorecard.yml` workflow had been red on every push to `master` for
  an extended period because
  `github/codeql-action/upload-sarif@b25d0ebf40e5...` was an "imposter
  commit" (a SHA that no longer exists in the action's repository, which
  Scorecard's webapp rejects as a tag-rotation defence). Repinned to the
  current `v4` head `e46ed2cbd01164d986452f91f178727624ae40d7`. The
  aggregate score (6.2 / 10) is unchanged by this PR; the fix unblocks
  the workflow so the score becomes a live signal instead of a stuck
  red X. ADR-0263 + research digest 0053 document the per-check
  breakdown, accepted blockers (`Code-Review`, `Branch-Protection`,
  `Maintained`, `CII-Best-Practices`), and the active remediation
  queue (Vulnerabilities, Pinned-Dependencies, Fuzzing, Signed-Releases,
  Packaging) for follow-up PRs.
