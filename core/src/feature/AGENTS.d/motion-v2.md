---
paths:
  - core/src/feature/integer_motion_v2.c
  - core/src/feature/motion_tools.h
invariant: Motion v2 option-surface parity, five-frame window on prev_prev_ref, and NEON shift semantics.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Motion v2 Option Surface and NEON Shift Semantics

- **`motion_v2` public option-surface duplicates motion v1**
  (fork-local, ADR-0337):
  [`integer_motion_v2.c`](../integer_motion_v2.c) registers its own
  `VmafOption[]` table for seven motion knobs
  (`motion_force_zero`, `motion_blend_factor`, `motion_blend_offset`,
  `motion_fps_weight`, `motion_max_val`, `motion_five_frame_window`,
  `motion_moving_average`) — duplicating motion v1's
  [`integer_motion.c`](../integer_motion.c) surface byte-for-byte
  against upstream Netflix `4e469601`. duplication is
  deliberate; v1 and v2 are independent extractors with independent
  output namespaces (`VMAF_integer_feature_motion*_score` vs
  `…_v2_score`). On rebase: when touching one extractor's option
  help string, touch other; when upstream touches option
  table, port change to **both** extractors. ADR-0141 catches
  drift on next edit.
- **`motion_v2` five-frame window = upstream `a2b59b77`** (ADR-1478, ends
  ADR-0337 deferral): `motion_five_frame_window=true` -> `extract()` takes
  SAD of frame n against `fex->prev_prev_ref` (n-2), frames 0 and 1 report 0,
  `flush()` uses `min_idx = stride = 2` (`motion2_at()`: `min(SAD[n-1],
  SAD[n+1])`, last frame its own SAD, frame 2 `SAD[3]`). Text = last upstream
  `integer_motion_v2.c` (`a4a1492d^`; upstream deleted file in `a4a1492d`,
  fork keeps extractor). Same arithmetic as `integer_motion.c`; change one
  -> change both. Never bring back `-ENOTSUP` in `init()` or constant
  `min_idx = 1`. No frame n-2 at index >= 2 -> `-EINVAL`, never empty
  picture read. `reads_prev_prev_ref()` answers option, as in
  `integer_motion.c` (framework keeps n-2 only for reader, ADR-1478).
  `motion_v2_cuda` / `_sycl` / `_hip` compute window and flush through
  same function (ADR-1491); `motion_v2_metal` does not declare
  option: model dispatch computes it on CPU (ADR-1359).
  Guards: `core/test/test_motion_five_frame_window.c`
  (scores against three-frame SAD of frame pairs (n-2, n), threads, pool
  of four, twin verdicts), `test_integer_motion_v2_coverage`.
- **`motion_v2` NEON shift semantics** (fork-local, ADR-0145):
  [`arm64/motion_v2_neon.c`](../arm64/motion_v2_neon.c) uses
  **arithmetic** right-shift throughout (`vshrq_n_s64(v, 16)` for
  Phase-2 known shift, `vshlq_s64(v, -(int64_t)bpc)` for
  Phase-1 runtime shift). fork's AVX2 variant
  [`x86/motion_v2_avx2.c`](../x86/motion_v2_avx2.c) uses
  `_mm256_srlv_epi64` (*logical*) which can diverge from scalar on
  negative-diff pixels. NEON matches scalar, AVX2 does not — this
  is intentional until AVX2 audit lands. On rebase: keep
  arithmetic-shift form in NEON; do NOT port AVX2's logical pattern
  even if it looks simpler. 4-lane stride + scalar tails on both
  sides of row are load-bearing for x_conv edge-mirror
  contract. See
  [ADR-0145](../../../../docs/adr/0145-motion-v2-neon-bitexact.md)
  and [rebase-notes 0038](../../../../docs/rebase-notes.md).
