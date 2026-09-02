<!-- markdownlint-disable MD013 MD060 -->
# Research-0089: Upstream sync coverage — 2026-05-08

- **Status**: Draft
- **Date**: 2026-05-08
- **Author**: @Lusoris
- **Tags**: upstream-sync, port, fork-local

## Question

Where does the lusoris fork stand vs Netflix/vmaf upstream as of
2026-05-08 — what's recently been added upstream that we don't have,
what's already silently ported, what's noise?

## Method

Per the [`/sync-upstream`](../../.claude/skills/sync-upstream/SKILL.md)
skill's port-only-topology procedure:

1. **Pre-flight**: `git merge-base master upstream/master` returns
   empty → port-only confirmed (expected for the fork's
   cherry-pick-then-squash workflow). No bare merge attempted.
2. **Pass 1 (subject match)**: scan the last 50 upstream commits;
   classify as `PORTED` if the subject line appears verbatim on fork
   master. **Result: 0 / 49 ported by subject.** That's expected: the
   fork's port-history uses different conventions
   (`feat: port upstream X (...)`), not a verbatim copy of the upstream
   subject.
3. **Pass 2 (content-hash similarity)**: for each `UNPORTED` commit,
   extract the *discriminating* identifiers it introduces (function
   defs, type defs, macros — filter out common keywords) and probe
   fork master for their presence. ≥ 80 % present → `PORTED-SILENTLY`.
   50–80 % → `PARTIAL`. < 50 % → genuinely `UNPORTED`. < 3 idents in
   the commit → `SKIP-doc-or-format` (heuristic limit; needs human
   triage).

## Findings

| Status | Count | Action |
|---|---|---|
| **UNPORTED** | 2 | Real gaps; recommend `/port-upstream-commit <sha>`. |
| **PARTIAL** (50–80 %) | 5 | Likely incremental optimisations on top of code we already have; eyeball before porting. |
| **PORTED-SILENTLY** | 1 | Already in fork. Commit subject in our master doesn't cite the upstream SHA — maintainer can decide whether to backfill the citation. |
| **SKIP-doc-or-format** | 41 | Heuristic limit; needs human triage by file area. Mix of motion_v2 option additions, cambi SIMD refactors, python-test cleanup, doc updates, merge commits. |
| **Total surveyed** | 49 | |

### UNPORTED — recommend `/port-upstream-commit`

- `984f281f` — `feature/cambi: fuse sliding-window subtract+add into uh_slide/uh_slide_edge`
- `9fad7317` — `feature/cambi: factor 2D SAT recurrence into 1D row prefix-sum + column add`

Both are cambi performance optimisations. Cambi has CUDA + SYCL +
SIMD twins on the fork, so each `/port-upstream-commit` call must
either update every twin or list the gap in `Known follow-ups`
(per CLAUDE.md §12 r12 + ADR-0141).

### PARTIAL — incremental on top of present code

- `1091b0c1` — `feature/cambi: add decimate_avx2`
- `933cccb4` — `feature/cambi: frame-level calc_c_values dispatch`
- `767a6780` — `feature/cambi: refactor calculate_c_values_row, add calculate_c_values_row_avx2`
- `bd278ea6` — `feature/cambi: add filter_mode_avx2`
- `41bacc83` — `feature/cambi: move shared code to cambi.h`

These add new AVX2 paths or refactor for the AVX2 dispatch. The fork
already has `feature/x86/cambi_avx2.c` (different content) so each
needs careful three-way merge. Recommend porting after the UNPORTED
two.

### PORTED-SILENTLY — already in fork

- `c17dd898` — `libvmaf/motion_v2: add motion_max_val`

Fork master has `motion_max_val` semantics but doesn't cite this SHA
in its commit subject. Optional cleanup: backfill the citation in the
fork's commit message, or note the silent port in
[`docs/rebase-notes.md`](../rebase-notes.md).

### SKIP-doc-or-format — needs human triage

41 commits the heuristic couldn't classify because they don't
introduce new top-level identifiers. **The heuristic is the limit
here, not the commits' importance.** A few are substantive:

- `4e469601` — `libvmaf/motion_v2: port remaining options` — option
  plumbing that almost certainly *isn't* on the fork yet (we ported
  motion_v2 selectively).
- `856d3835` — `libvmaf/motion_v2: fix mirroring behavior, since a44e5e61` — a real bug fix on a feature we ship.
- `a2b59b77` — `libvmaf/motion_v2: add motion_five_frame_window` — new
  option; needs porting if we want feature parity.
- `77474251`, `8c60dc9e`, `d655cefe`, `721569bc` — cambi
  refactors / option additions / docs.
- `662fb9ce` — `python: replace polling-based workfile synchronization with semaphores` — fork's `python/vmaf/workspace/`
  diverges, so this needs a careful three-way before porting.
- The `python/test/MyTestCase` series (10+ commits) — Netflix's test
  harness adoption. Touches files the fork's
  [Netflix-golden-data gate](../../docs/adr/0024-netflix-golden-preserved.md)
  pins. Port carefully — the assertion values are immutable.

The remaining ~20 `SKIP` rows are `Merge pull request #...`
commits that flatten the upstream PR boundaries; not portable in
isolation.

## Recommendation

Port in this order, one PR per `/port-upstream-commit` invocation
(per the skill's guidance — bulk merge is forbidden under port-only
topology):

1. **`856d3835`** (motion_v2 mirroring fix) — bug fix, smallest blast
   radius, validates the workflow.
2. **`a2b59b77`** (motion_five_frame_window) — new option; trivial to
   wire if the predecessor commits are already present.
3. **`4e469601`** (motion_v2 port remaining options) — option-API
   surface; affects libvmaf public API. ADR-0186 ffmpeg-patches replay
   is required (§14 hard rule).
4. **`984f281f` + `9fad7317`** (cambi UNPORTED algorithmic
   optimisations) — paired; must update CUDA / SYCL / SIMD twins.
5. **PARTIAL cambi commits (5)** — only after the two UNPORTED ones
   above land, because they build on the same dispatch table.

Defer indefinitely:

- All 20+ `Merge pull request` commits — captured by the squash-
  ports of the underlying changes once those land.
- Python `MyTestCase` series — touches Netflix-golden-tied test files;
  the fork's per-test reformatting cost outweighs the benefit while
  the golden-gate is the source of numerical truth.

## References

- [/sync-upstream skill](../../.claude/skills/sync-upstream/SKILL.md)
- [/port-upstream-commit skill](../../.claude/skills/port-upstream-commit/SKILL.md)
- [ADR-0028 — ADR maintenance rule](../adr/0028-adr-maintenance-rule.md)
- [ADR-0141 — touched-file cleanup rule](../adr/0141-touched-file-cleanup-rule.md)
- [ADR-0186 — Vulkan image-import impl + ffmpeg-patches replay](../adr/0186-vulkan-image-import-impl.md)
- User direction (2026-05-08, this session): "we have a shitton of new upstream commits"
