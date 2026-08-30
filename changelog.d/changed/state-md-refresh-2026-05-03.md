- `docs/state.md` refresh 2026-05-03. Bumped header date
  (2026-04-29 → 2026-05-03). Closed Issue #239 (FFmpeg
  `libvmaf_vulkan` filter wall-clock serialisation) by moving the
  Open-bugs row to "Recently closed" with PR #241 / commit
  `e266bf8e` and ADR-0251 (renumbered from 0235 in PR #310 dedup
  sweep) — the `v2 ≤ 0.7 × v1` measurement gate flipped ADR-0251
  to Accepted. Added a new Open-bugs row for the
  `y4m_convert_411_422jpeg` heap-buffer-overflow surfaced by the
  PR #348 libFuzzer scaffold (reproducer parked at
  `core/test/fuzz/y4m_input_known_crashes/y4m_411_w2_h4_oob_dst.y4m`,
  fix follow-up TBD). Audited "Recently closed" for stale draft-PR
  refs: six rows updated to cite merged commit SHAs and slug-correct
  ADR refs (ADR-0246 for the kernel-template, not 0221 which is now
  `changelog-adr-fragment-pattern.md`). Refreshed Netflix#955
  deferred-row last-checked stamp (Netflix#1494 still `state=OPEN`
  per gh API). No row removed below its closure threshold; "Update
  protocol" section untouched. Per ADR-0165 / CLAUDE.md §12 r13.
