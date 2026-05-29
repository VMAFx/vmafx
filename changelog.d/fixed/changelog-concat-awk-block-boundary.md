- **`concat-changelog-fragments.sh` awk block-boundary bug fixed** — the
  `--check` and `--write` modes used `/^## [^[]/` to detect the end of the
  `[Unreleased]` block; this pattern terminated early on any `## SectionTitle`
  line embedded in a fragment (e.g. `## Added`, `## What ships`, `## [perf] …`).
  Switched to `/^## \[(Unreleased|[0-9])/` which matches only actual Keep-a-Changelog
  versioned-release and `[Unreleased]` headers, making `--check` deterministic.
  32 perf-category fragments (`changelog.d/perf/` × 27 + `changelog.d/performance/`
  × 5) consolidated into the recognised `changed/` section with `perf-` prefix
  per ADR-0221. Three duplicate stubs removed
  (`ort-run-stack-arrays-f3b.md`, `adm-pnorm-deferred-comment.md`,
  `vmaf-tune-predictor-directory-corpus.md` in `fixed/`).
  One filename collision renamed (`fr-regressor-v3-namespace.md` →
  `fr-regressor-v3-namespace-adr-appendix.md` in `changed/`).
