### Renovate: Go/Cargo grouping, off-hours schedule, concurrent-PR cap (ADR-0812)

- Global `schedule` changed from `"at any time"` to `"before 6am on weekdays"` (Europe/Vienna) so dependency PRs no longer compete with active work for CI slots. Vulnerability alerts retain their existing `"at any time"` override.
- Go (`gomod`) minor + patch updates are now grouped into a single weekly PR auto-merged on early Monday; major bumps remain individual and require human review.
- Cargo (`cargo`) minor + patch updates follow the same group-and-auto-merge pattern; major bumps are manual.
- `prConcurrentLimit` reduced from 12 → 10 now that Go grouping compresses many per-package PRs into one.
