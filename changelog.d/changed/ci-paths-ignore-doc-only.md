- CI — `libvmaf-build-matrix.yml` and `tests-and-quality-gates.yml`
  now carry a `paths-ignore` filter on their `pull_request` triggers:
  `docs/**`, `**/*.md`, `changelog.d/**`, `CHANGELOG.md`,
  `.workingdir2/**`. Doc-only / research-only PRs no longer fire the
  18-cell build matrix or the 10-job test matrix. Safe under
  ADR-0313: the Required Checks Aggregator already treats a
  workflow-not-reported as path-filter-skipped/acceptable, so branch
  protection still passes. Mirrors the path-filter pattern from
  ADR-0317 on `docker-image.yml` and `ffmpeg-integration.yml`. Saves
  roughly 14 runner-min per average PR, with bigger wins on doc-only
  PRs (full ~14 min wall-clock skipped). See
  [ADR-0341](../../docs/adr/0341-ci-paths-ignore-doc-only-prs.md) and
  [Research-0089 §3.2](../../docs/research/0089-ci-cost-optimization-audit-2026-05-09.md).
