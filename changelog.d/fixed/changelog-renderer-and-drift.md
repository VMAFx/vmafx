- **CHANGELOG.md renderer splice contract** — `scripts/release/concat-changelog-fragments.sh`
  used `^## [^[]` as the "end of Unreleased block" sentinel, which any
  fragment-internal `## ` header tripped. Each `--write` cycle inflated
  `CHANGELOG.md` by ~3 kB; master tip `544299fae1` had drifted to
  **59 757 lines** (~95 % duplicated content). Boundary anchored on
  `^## \[` instead (release-please's only header shape), the renderer
  now demotes stray fragment-body `# ` / `## ` to `**bold**`
  pseudo-headers, fragments outside the Keep-a-Changelog section set
  emit a stderr WARNING (per PR #384 / ADR-0892), and empty fragments
  are skipped with a WARNING. Companion: source-side normalisation of
  102 in-tree fragments (strip first-line section-name headers, demote
  remaining `## ` to `### `) + relocation of `changelog.d/perf/` +
  `changelog.d/performance/` into `changelog.d/changed/perf-*.md`.
  `CHANGELOG.md` regenerated to **15 030 lines** — a 44 727-line
  reduction; `--write` is idempotent across consecutive runs; future
  release-please `## [vX.Y.Z]` sections are preserved across re-renders.
  See [ADR-0913](docs/adr/0913-changelog-renderer-splice-contract.md)
  and [Research-0913](docs/research/0913-changelog-renderer-and-drift-2026-05-31.md).
