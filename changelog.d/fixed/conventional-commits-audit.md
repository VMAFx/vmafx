- **CHANGELOG-fragment renderer no longer silently drops performance entries
  (ADR-0892).** Moved 32 fragments from the undocumented `changelog.d/perf/`
  (27 files) and `changelog.d/performance/` (5 files) directories — both
  silently skipped by `scripts/release/concat-changelog-fragments.sh` — into
  `changelog.d/changed/` with a `perf-` filename prefix so they sort together
  inside the rendered `### Changed` section. Stripped redundant
  `### Performance` / `## perf(…)` in-fragment headings; demoted other body
  headings to bold-prefixed bullets so they nest cleanly under `### Changed`.
  Also extended `release-please-config.json` (root + `ai` packages) with
  section mappings for the three standard Conventional-Commits types that
  had no section assigned — `revert → Reverts`, `security → Security`,
  `style → Style` — so a `revert:` / `security:` / `style:` commit on master
  no longer falls through release-please's section assignment. The six
  Keep-a-Changelog directories (`added`, `changed`, `deprecated`, `removed`,
  `fixed`, `security`) are now the only valid fragment-tree sections; the
  `changelog.d/README.md` calls this out explicitly. Audit ran against
  `master@83698bd5b2` and found zero Conventional-Commits format violations
  in the last 50 commits — only the two consistency gaps above. See
  [Research-0892](../docs/research/0892-conventional-commits-audit-2026-05-30.md).
