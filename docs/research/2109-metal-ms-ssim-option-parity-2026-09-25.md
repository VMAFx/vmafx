<!-- markdownlint-disable MD013 MD060 -->
# Research-2109: Metal float_ms_ssim option and score parity — 2026-09-25

**Status:** Complete

**Authority inspected:** signed commit `71c3c1557` (agent/metal-ms-ssim-option-gap)

**Scope:** Metal `float_ms_ssim` option parsing (`enable_db`, `clip_db`, `enable_chroma`, `enable_lcs`), geometry-derived `max_db` ceiling, 3-plane chroma computation and emission (`float_ms_ssim_cb`, `float_ms_ssim_cr`), subsampled chroma min-dimension validation (>= 176), and fail-closed score emitter wiring under ADR-1221. Device-free contract tests and Apple Silicon parity test scaffolding. No benchmark, tuning, retraining, or Netflix golden assertion changes.

## Problem Statement

Pre-RC1 audit gap `T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07` in `docs/state.md` noted:
While CPU `float_ms_ssim.c`, SYCL `integer_ms_ssim_sycl.cpp`, CUDA `integer_ms_ssim_cuda.c`, and HIP `integer_ms_ssim_hip.c` expose `enable_db`, `clip_db`, and `enable_chroma`, the Metal twin `core/src/feature/metal/float_ms_ssim_metal.mm` only exposed `enable_lcs`. Any attempt to request dB-domain scoring or chroma planes on Metal failed during option parsing with `-EINVAL`. Furthermore, `collect_fex_metal` passed hardcoded `false, INFINITY` to `vmaf_ms_ssim_emit_scores()`.

## Parity Architecture & Implementation

1. **Option Registration**:
   `enable_db`, `clip_db`, and `enable_chroma` added as `VMAF_OPT_TYPE_BOOL` with default `false` in `options[]`.

2. **Per-Plane Geometry and Buffers**:
   - `MS_SSIM_MAX_PLANES 3` defined.
   - `MsSsimPlaneGeometryMetal` tracks per-scale dimensions, grid sizes, and workgroup partial block counts for each plane.
   - For `enable_chroma = false` (or YUV400P), `n_planes = 1`. For `enable_chroma = true`, `n_planes = 3`.
   - Separate pyramid buffers (`pyramid_ref[p][scale]`, `pyramid_cmp[p][scale]`) and partials buffers (`l_partials[p][scale]`, `c_partials[p][scale]`, `s_partials[p][scale]`) allocated for each active plane.
   - `alloc_metal_buffers` and `release_metal_buffers` cleanly handle all planes with complete error recovery and deallocation on failure.

3. **Chroma Dimension Validation**:
   - The 5-level 11-tap pyramid requires every dimension to be >= 11 * 2^4 = 176.
   - `check_chroma_min_dim` enforces that subsampled chroma planes (e.g. 4:2:0 halved horizontally and vertically) are >= 176x176. For YUV420P, luma must be >= 352x352. If smaller, returns `-EINVAL` with an informative error log.

4. **dB Conversion and `max_db` Ceiling (ADR-1221)**:
   - When `clip_db` is enabled: `max_db = ceil(10. * log10(peak * peak / mse))` where `peak = (1 << bpc) - 1` and `mse = 0.5 / (w * h)`.
   - When `!clip_db`: `max_db = INFINITY`.
   - In `collect_fex_metal`, scores are validated with `vmaf_ssim_prepare_score_named(name, raw_score, s->enable_db, s->max_db, index, &score)`.
   - Scores emitted using `vmaf_ms_ssim_emit_scores` (plane 0) and `vmaf_ssim_emit_score_named` (planes 1 and 2), passing `s->enable_db, s->max_db`.

5. **Dispatch Strategy & Features**:
   - `provided_features` in `float_ms_ssim_metal.mm` updated with `"float_ms_ssim"`, `"float_ms_ssim_cb"`, `"float_ms_ssim_cr"`.
   - `g_metal_features` in `core/src/metal/dispatch_strategy.c` updated to register `"float_ms_ssim_cb"` and `"float_ms_ssim_cr"`.

6. **NASA Rule 4 Adherence**:
   - All helper functions decomposed so every function is <= 60 LOC and cyclomatic complexity <= 10.
   - Validated by unit test in `test_metal_ms_ssim_options_contract.py`.

## Verification & Test Matrix

- **Device-Free Contract Suite**:
  - `core/test/test_metal_ms_ssim_options_contract.py`: 8 test methods checking option types, defaults, cross-twin parity, provided features, dispatch strategy, max_db formula across 8/10/12/16-bit depths, chroma min-dim logic, score emitter wiring, and NASA Rule 4 LOC bounds. Registered under `fast` test suite in `core/test/meson.build`.
  - `core/test/test_nonfinite_collector_wiring.py`: updated with `metal/float_ms_ssim_metal.mm` in `REQUIRED["vmaf_ssim_prepare_score_named"]` and option-aware `METAL_MS_SSIM_OPTIONS_DB_CALL`.
- **Metal Parity Suite**:
  - `core/test/test_metal_float_ms_ssim_parity.c`: upgraded fixture to 512x384 (chroma 256x192 >= 176). Added `test_metal_float_ms_ssim_clip_db_ceiling` and `test_metal_float_ms_ssim_parity_chroma`. Skips honestly off Apple hardware (`[skip: no Metal device]`).
- **Repository Gates**:
  - `meson test -C build --suite=fast`: 164 passing tests, 0 failed, 1 skipped.
  - `scripts/ci/check-state-md-rows.sh`: 0 errors.
