- `docs/state.md` audit + backfill 2026-05-08
  ([Research-0086](docs/research/0086-state-md-audit-2026-05-08.md)).
  Bumped header date (2026-05-06 → 2026-05-08). Backfilled four
  missing closure rows for bug-fix PRs that did not touch
  `docs/state.md` in the same PR (against CLAUDE.md §12 r13 /
  ADR-0165): PR #391 (CUDA `integer_motion_cuda` last-frame
  duplicate-write warning + `context could not be synchronized`
  regression introduced by PR #312's fence batching), PR #389
  (`vmaf-tune` Phase A corpus pipeline emitted `vmaf_score=NaN` on
  every encoded clip — `run_score` handed `.mp4` directly to
  libvmaf CLI), PR #390 (CUDA build broken on dev hosts with gcc
  16.x — `nvcc --std c++20` for libstdc++ char8_t support), PR
  #234 (FFmpeg `vf_libvmaf` build break under `VK_NO_PROTOTYPES`
  against `release/8.1`). Audit also fixed three stale rows in
  the "Deferred (waiting on external trigger)" section: removed
  the duplicate Netflix#955 entry (older row preserved the newer
  2026-05-03 last-checked stamp), removed the stray `|---|---|---|---|`
  Markdown table separator that was breaking the table layout
  mid-section, and removed the duplicate **T-VK-1.4-BUMP** row
  that conflicted with the canonical Open-bugs row at the top of
  the file (PR #346 promoted the item from Deferred to Open-with-
  partial-fix; the Deferred clone was never deleted). Two cases
  flagged for maintainer disposition rather than guessed: the
  Tiny-AI C1 baseline T6-1a "TRIGGERED 2026-04-29" row whose
  closure section depends on whether the baseline has actually
  trained, and the convention-question whether pure upstream-port
  PRs (#301, #302, #303, #315) belong in `docs/state.md` at all.
  Per ADR-0165 / CLAUDE.md §12 r13.
