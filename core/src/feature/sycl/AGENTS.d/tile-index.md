---
paths:
  - core/src/feature/sycl/sycl_tile_index.h
  - core/src/feature/sycl/integer_vif_sycl.cpp
invariant: Tile loaders clamp after reflecting (sycl_tile_index.h) around borders.
---
<!-- markdownlint-disable MD013 MD060 -->
# Tile loader coordinates and boundary reflection

- **Tile loaders clamp after reflecting (`sycl_tile_index.h`)**
  (T-SYCL-TILE-HALO-OOB-READ-2026-09-29, Research-2123). Fixed-size SLM tiles
  load padding lanes too; one reflection of those leaves plane on small
  frames (ADM vertical DWT: row 16 of 8-row plane -> -1) and reads outside
  USM buffer -> `UR_RESULT_ERROR_DEVICE_LOST` when page is unmapped.
  Every single-reflection loader wraps reflected index in
  `vmaf_sycl_tile_index()`: `integer_adm` `launch_dwt_vert_pair`, `integer_vif`
  `dev_vert_load_tile` + `dev_fused_load_tile`, `integer_motion_pipeline`
  `load_diff` (motion + motion_v2), `float_motion`
  `fm_load_tile`, `float_vif` `load_vif_tile`. Identity for consumed samples ->
  no score change. New tiled kernel = same wrap. Per-output reflections
  (`dev_hori_convolve_border`, float VIF decimate) are consumed-only and stay
  unwrapped; each extractor's minimum frame size keeps them in plane
  (integer VIF: `VIF_MIN_DIM` above). Guards: `test_sycl_adm_tiny_frames`,
  `test_sycl_vif_min_dim`.
