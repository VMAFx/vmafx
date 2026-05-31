<!-- markdownlint-disable MD038 MD060 -->
# Research-0913 — CHANGELOG.md renderer splice bug + 23 k+ line drift audit

- **Date**: 2026-05-31
- **Author**: lusoris (delegated to Claude Code agent)
- **Companion ADR**: [ADR-0913](../adr/0913-changelog-renderer-splice-contract.md)
- **Companion PR**: (this branch — `fix/changelog-renderer-and-drift`)

## TL;DR

`scripts/release/concat-changelog-fragments.sh` had a splice-contract
bug that inflated `CHANGELOG.md` by ~3 kB per `--write` cycle for the
last several merge windows. On master tip `544299fae1` the file was
**59 757 lines**; this audit + fix returned it to **15 030 lines**, a
44 727-line reduction. The fix is a one-character regex change in the
boundary sentinel (`^## [^[]` → `^## \[`) plus 102 fragment
normalisations + 32 directory-relocations + an unknown-subdir warning
guard.

## Symptom timeline

- **PR #332**: reviewer flagged "pre-existing CHANGELOG.md drift in
  the diff" — not rooted, merged with the drift in place.
- **PR #383**: same flag, same response.
- **PR #401**: same flag, same response.
- **PR #384 / ADR-0892**: identified two adjacent issues (Conventional
  Commits config gaps + `changelog.d/perf/` + `changelog.d/performance/`
  silently skipped by the renderer). Moved 32 fragments to
  `changelog.d/changed/perf-*.md`. Explicitly **deferred** the
  CHANGELOG.md regeneration to "a separate sweep PR can run `--write`
  once this lands and the fragment tree is invariant-clean." PR #384
  has not merged at time of this dossier (Required Checks Aggregator
  FAILURE on the last run; OPEN).

## Root cause — the splice sentinel

The renderer splices the rendered Unreleased body into `CHANGELOG.md`
between the `## [Unreleased] ...` header and the next "release"
section header. Pre-fix, the awk pattern was:

```awk
in_block && /^## [^[]/ {in_block=0}
```

That regex means: "any h2 that does not begin with an open square
bracket." Intent: skip `## [Unreleased]` and `## [vX.Y.Z]`, treat any
other `## Foo` as the start of release history.

The assumption: fragment bodies are *bullets*, not *headers*. The
README of `changelog.d/` even states "Write a Markdown bullet (or a
small block of bullets)."

The reality: **84 of 102 fragments** (~82 %) violated that
assumption. Examples:

- `changelog.d/fixed/vmaf-tune-fast-nr-nav-gap.md` opened with
  `## Fixed` (redundant — the renderer emits `### Fixed` itself).
- `changelog.d/changed/vulkan-submit-pool-pr-c.md` opened with
  `## Vulkan submit-pool migration PR-C (ADR-0354)`.
- `changelog.d/added/0608-zed-editor-project-config.md` opened with
  `# zed-editor project config (ADR-0608)` (h1).

When such a fragment got spliced into `CHANGELOG.md`, the next
`--check` (or `--write`) parsed the file, hit the in-body `## Vulkan
submit-pool ...`, and treated the rest of the rendered body **as
release history to preserve**. The next `--write` re-rendered the
fragments at the top of the body, but the awk pass-1 had already
preserved the dangling tail. Each cycle: header + new render + old
render (preserved as tail) + further-old render (further tail) = ~3 kB
growth per `--write` run.

## Quantitative audit

| Metric | Pre-fix | Post-fix | Delta |
|---|---|---|---|
| `CHANGELOG.md` lines | 59 757 | 15 030 | −44 727 |
| `^## ` headers in CHANGELOG.md | 155 | 1 | −154 |
| `### Section` headers post-legacy in CHANGELOG.md | 113 | 6 | −107 |
| Fragments with leading `## ` | 84 | 0 | −84 |
| Fragments with redundant `### Section` first-line header | 18 | 0 | −18 |
| `changelog.d/<dir>/` outside known set | 2 (`perf/`, `performance/`) | 0 | −2 |
| `--write` idempotency | broken (cycle-amplifies) | clean | — |

## Fix shape

1. **`scripts/release/concat-changelog-fragments.sh`**:
   - Boundary regex centralised in `BOUNDARY_REGEX='^## \\['` and
     plumbed through awk's `-v boundary=` so both passes share the
     same definition.
   - `emit_fragment()` helper demotes leading `# ` / `## ` to
     `**bold**` pseudo-headers at render time — defense-in-depth even
     if a future fragment regresses.
   - `warn_unknown_subdirs()` emits a stderr WARNING for each
     `changelog.d/<dir>/` outside the Keep-a-Changelog set (skip the
     fragments inside, but make the skip visible).
   - Empty-fragment skip with stderr WARNING.
   - Doc block updated with the splice-contract explanation +
     fragment-hygiene note + pointer to ADR-0913.

2. **Fragment normalisation** (102 files touched):
   - First-line `## Section` matching the parent directory: **deleted**
     (renderer emits `### Section` itself).
   - First-line `### Section` matching the parent directory: **deleted**
     (same reason; 18 cases).
   - Remaining `## ` headers in fragment bodies: **demoted** to `### `
     so the source tree matches the splice contract without relying
     on render-time demotion.

3. **Directory cleanup**:
   - `changelog.d/perf/*.md` (27 files) → `changelog.d/changed/perf-*.md`
   - `changelog.d/performance/*.md` (5 files) → `changelog.d/changed/perf-*.md`
   - `changelog.d/perf/` and `changelog.d/performance/` removed.
   - This overlaps with PR #384's intent; whichever PR merges first
     no-ops the rename for the other.

4. **CHANGELOG.md regeneration**: `bash
   scripts/release/concat-changelog-fragments.sh --write`. Output is
   bit-identical across two consecutive runs (idempotent).

## Release-history preservation (smoke test)

Simulated a future release-please commit by appending
`## [3.0.0-lusoris.1] - 2026-06-01` + a body to CHANGELOG.md, then
re-ran `--write`. Result: the appended `## [version]` section
survives unchanged; only the `[Unreleased]` block above it gets
rewritten. The contract holds for the real release-please workflow.

## Reproducer

```bash
# Verify the bug on master (pre-fix)
git checkout 544299fae1
bash scripts/release/concat-changelog-fragments.sh --check
# Exit 0 — but the file is 59 757 lines (legitimate fragment body
# is ~3 k lines). The check is "self-similar duplicates compare
# equal."

# Apply this branch
git checkout fix/changelog-renderer-and-drift

# Verify the fix
wc -l CHANGELOG.md                                             # 15 030
bash scripts/release/concat-changelog-fragments.sh --check     # exit 0
bash scripts/release/concat-changelog-fragments.sh --write     # idempotent
diff <(cat CHANGELOG.md) <(bash scripts/release/concat-changelog-fragments.sh --write 1>&2 && cat CHANGELOG.md)
# (empty diff — second --write changes nothing)
```

## Out of scope

- Adding a `--lint` mode that *fails* on stray `## ` in fragment
  bodies. Render-time demoter is enough; lint is incremental polish.
- Restructuring the multi-section fragments (e.g.
  `changelog.d/added/0550-tiny-model-auto-resize.md` contains both
  Added and Fixed entries in one file). The renderer accepts them as
  authored; an authoring-policy fix is a separate concern.
- Wiring the `--check` lane as a required CI status (ADR-0892's
  Consequences list this as a follow-up).

## References

- ADR-0913 (companion)
- ADR-0221 (original fragment system)
- ADR-0892 (perf/ + performance/ hygiene; in-flight PR #384)
- PR #332, #383, #401 (drift symptoms not rooted)
- Source: `req` (user briefing 2026-05-31)
