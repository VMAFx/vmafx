## Docs hygiene bundle (#127, #216, #291, #233)

- **NOLINT cluster audit and refactor plan (ADR-0780, #127)**: swept all 218 NOLINT
  annotations in `core/src/` for clusters of five or more identical suppressions per
  file; identified five clusters (71 annotations total); defined three-PR refactor
  sequence (SYCL stride explicit casts, GPU slab `SLAB_FIELD` macro, ADM band-size
  `NOLINTBEGIN` consolidation). Research digest: `docs/research/nolint-cluster-audit-2026-05-29.md`.

- **Per-surface doc compliance audit (ADR-0848, #216)**: audited 30 most recent merged
  commits (PRs #96–#174) for CLAUDE §12 r10 compliance; score 22/30 N/A, 5/8 with
  surface changes compliant (62.5 %); three confirmed gaps tracked as open bugs
  (T-DOC-VULKAN-STALE-POST-ADR0726, T-DOC-LEGACY-RUNNER-MISSING-DEPRECATION, PR #135
  log format). `docs/state.md` updated.

- **`docs/state.md` drift sweep (#291)**: closed 5 stale Open rows — T-LEGACY-RUNNER-ANSNR-BROKEN,
  T-LEGACY-RUNNER-STUB-MISSING-2026-05-29 (both fixed by ADR-0749 + PR #283), and
  T-VK-1.4-BUMP, T-VK-CIEDE-F32-F64, T-VK-VIF-1.4-RESIDUAL-ARC (all superseded by
  ADR-0726 Vulkan backend drop). Also closed T-DOC-VULKAN-STALE-POST-ADR0726 and
  T-DOC-LEGACY-RUNNER-MISSING-DEPRECATION (resolved by ADR-0749/ADR-0726 closures above).

- **Changelog fragment concat fix (ADR-0221, #233)**: `concat-changelog-fragments.sh`
  block-boundary awk regex fixed (`/^## [^[]/` → `/^## \[(Unreleased|[0-9])/`); 32
  `changelog.d/perf/` + `changelog.d/performance/` fragments relocated to
  `changelog.d/changed/perf-*.md`; 3 duplicate stubs removed; `CHANGELOG.md`
  regenerated from 59 757 → 14 793 lines; `--check` exits 0.
