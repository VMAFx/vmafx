- Active monitoring for the Netflix#955 / Netflix#1494 upstream
  deferral via the new `.github/workflows/upstream-netflix-955-watcher.yml`
  workflow. Runs every Sunday 06:00 UTC, polls `Netflix/vmaf#1494`,
  and opens a fork-side tracking issue (label `upstream-merged`)
  when the upstream PR merges so the `docs/state.md` row can actively
  close. Replaces the previous "scheduled remote agent re-runs
  weekly" comment, which had no in-tree audit trail. Per
  [ADR-0448](../docs/adr/0448-active-upstream-monitoring-discipline.md)
  every deferral row that names an external trigger now has a matching
  watcher.
