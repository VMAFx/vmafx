<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# VIF, motion, PSNR, and moment: `core/src/feature/` agent invariants

vif*, integer_vif.c, float_vif.c, motion*, integer_motion*, float_motion.c,
psnr*, float_psnr*, integer_psnr.c, third_party/xiph/psnr_hvs.c, moment*, and
float_moment*.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `float_moment_sum.h`, `float_moment_sum_gpu.h` | [float-moment-sum](AGENTS.d/float-moment-sum.md) | float_moment twins form CPU's rounded 2nd-moment sum past 2^53 units; integers only, checked walk. |
| `moment.c`, `moment.h`, `arm64/moment_neon.c`, `arm64/moment_sve2.c` | [float-moment](AGENTS.d/float-moment.md) | compute_2nd_moment adds float squares into one double in raster order; every SIMD kernel returns its bits. |
| `float_psnr_rows.h` | [float-psnr-rows](AGENTS.d/float-psnr-rows.md) | float_psnr twins add each row's exact sum into double in CPU's row order. |
| `float_vif.c`, `vif_tools.h` | [float-vif](AGENTS.d/float-vif.md) | float_vif lint decomposition, run-time Gaussian filter construction, and minimum dimension checks. |
| `vif_tools.c`, `vif_tools.h` | [lanczos4](AGENTS.d/lanczos4.md) | lanczos4 prescale weights single implementation shared with GPU twins. |
| `integer_motion_v2.c`, `motion_tools.h` | [motion-v2](AGENTS.d/motion-v2.md) | Motion v2 option-surface parity, five-frame window on prev_prev_ref, and NEON shift semantics. |
| `motion_window.h`, `integer_motion.c`, `integer_motion_v2.c`, `feature_extractor.h`, `/core/test/test_motion_window_incremental.c`, `/core/test/test_motion_window_advance_contract.py`, `/core/test/test_score_pooled_eagain.c` | [motion-window-incremental](AGENTS.d/motion-window-incremental.md) | motion2/motion3 of frame i final once SADs 0..max(i+1,min_idx) in; same statements as flush; state carried. |
| `motion.c`, `float_motion.c`, `integer_motion.c` | [motion](AGENTS.d/motion.md) | Motion plane structures, upstream options, mirror implementations, and chroma min dims. |
| `third_party/xiph/psnr_hvs.c`, `psnr_hvs_score.c` | [psnr-hvs](AGENTS.d/psnr-hvs.md) | Masking threshold = upstream float product (ADR-1488); host scoring tail and SIMD DCT stay bit-exact. |
| `psnr.c`, `psnr.h`, `float_psnr.c`, `integer_psnr.c`, `psnr_score.h` | [psnr](AGENTS.d/psnr.md) | PSNR bucket lint shape, cross-backend enable_chroma parity, and uncapped options. |
| `integer_vif.c`, `vif_tools.c` | [vif](AGENTS.d/vif.md) | Integer VIF scalar reference tails, log2 table generation, score append order, and NULL preservation. |
