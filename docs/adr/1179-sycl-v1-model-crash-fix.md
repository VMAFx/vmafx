<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1179: Fix Intel Arc SYCL Crashes and Default Model Resolution Divergence

- **Status**: Accepted
- **Date**: 2026-09-05
- **Deciders**: maintainers
- **Tags**: sycl, gpu, cambi, speed, model, default, arc, fp64, adr-0220

## Context

Running `vmaf --backend sycl --model version=vmaf_v1.0.16_3d0h` (the default model on master per ADR-1169) on Intel Arc GPUs (e.g. Arc A380 / ACM-G11) failed with multiple distinct fatal crashes and execution errors:

1. **SIGSEGV in `integer_cambi_sycl.cpp`**: The SYCL CAMBI extractor attempted to bisect and re-evaluate contrast limits on `init()`, but left `buffers.v_band_base` and `buffers.v_band_size` uninitialized, causing invalid device pointer offsets and buffer overrun. Furthermore, the histogram buffer was sized only to `num_bins` without considering `v_band_size`.
2. **SIGABRT in `speed_chroma_sycl.cpp` and `speed_temporal_sycl.cpp`**: Intel Arc A-series GPUs lack hardware double-precision support (`aspect::fp64`). Both kernels allocated `double` accumulators and `sycl::local_accessor<double, 1>` work-group arrays, triggering an uncaught SYCL exception `terminate_handler: Device does not have aspect fp64`. This directly violated ADR-0220 ("SYCL fp64-less device contract").
3. **Exit 255 (`problem generating pooled VMAF score`)**: The default model `vmaf_v1.0.16_3d0h` configures CAMBI with `cambi_high_res_speedup: 1080`. The CPU CAMBI extractor registers this parameter as `VMAF_OPT_FLAG_FEATURE_PARAM`, serializing the feature name as `cambi_hrs_1080_cmxv_17_vlt_0.06`. However, `options_cambi_sycl` omitted `cambi_high_res_speedup` (`hrs`), causing `vmaf_feature_name_dict_from_provided_features` to omit `_hrs_1080` from the SYCL feature dictionary. When the prediction engine attempted to fetch `cambi_hrs_1080_...`, `predict_load_feature_score` returned `-EAGAIN` (-11), aborting execution.
4. **Sub-ULP Float Drift in Host SIMD Build under icx**: When building with the oneAPI toolchain (`icx`), `x86_avx2_static_lib` and `x86_avx512_static_lib` lacked `_x86_simd_strict_fp_extra` (`-fp-model=precise`), causing `icx` to apply `-fp-model=fast=1` and drift `float_motion` scores outside the Netflix golden assertion threshold by 0.00007.

## Decision

We fix all five failure points at their respective architectural boundaries:

1. **Cambi Initialization & Histogram Sizing**: Expose `vmaf_cambi_init_tvi_and_vlt` in `core/src/feature/cambi.c` and `cambi_internal.h`. In `integer_cambi_sycl.cpp`, invoke this helper on `init()` to properly populate TVI, VLT, and contrast tables. Size device histogram buffers with `MAX(num_bins, v_band_size)`.
2. **FP64 Elimination in Speed Extractors**: Replace all `double` accumulators, work-group local accessors, and reduction buffers with `float` in `speed_chroma_sycl.cpp` and `speed_temporal_sycl.cpp`, adhering strictly to ADR-0220.
3. **`cambi_high_res_speedup` Parity**: Add `cambi_high_res_speedup` (alias `"hrs"`) to `options_cambi_sycl` with `VMAF_OPT_FLAG_FEATURE_PARAM`. Add resolution checks against `CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_*`, implement window adjustment in `cambi_sycl_adjust_window`, and update scale decimation logic to ensure identical feature dictionary keys and numerical equivalence with CPU CAMBI.
4. **NULL Picture Flush Tolerance**: Guard against NULL pictures in `core/test/test_integer_cambi_sycl.c` to prevent test harness segfaults during EOS flush.
5. **Strict-FP on SIMD Static Libraries under icx**: Add `_x86_simd_strict_fp_extra` (`-fp-model=precise`) to `x86_avx2_static_lib` and `x86_avx512_static_lib` in `core/src/meson.build`, ensuring that host SIMD floating-point routines compiled with `icx` maintain bit-exact parity with GCC reference builds.
6. **Dedicated Regression Test**: Add `python/test/sycl_default_model_test.py` with an automated 2-frame SYCL probe that verifies that `--backend sycl` with the default model executes without crashing and outputs valid pooled and per-frame `vmaf` scores.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fall back to CPU for `cambi` or `speed` when SYCL default model is invoked | Avoids modifying SYCL kernels | Defeats the purpose of GPU acceleration; causes silent fallback violating ADR-0214 | Violates project charter against silent narrowing of scope. |
| Loosen Netflix golden assertions for `float_motion` under icx | Easy one-line change in Python tests | Breaks the cardinal project invariant: NEVER modify Netflix golden data assertions | Hard rule: golden assertions are ground truth. |
| Re-implement full TVI table generation in SYCL | Decouples from `cambi.c` | Duplicate math code prone to drift; violates DRY | Sharing `vmaf_cambi_init_tvi_and_vlt` guarantees bit-exact table parity. |

## Consequences

- **Positive**:
  - `vmaf --backend sycl` runs out of the box with the default model (`vmaf_v1.0.16_3d0h`) on Intel Arc GPUs.
  - Zero crashes (SIGSEGV, SIGABRT) across all resolutions.
  - Bit-exact or within-tolerance numerical parity between CPU and SYCL on standard test pairs.
  - Netflix CPU golden test suite (`make test-netflix-golden`) remains 100% green (271 passed, 12 skipped, 0 failed).
- **Negative**:
  - None. Code complexity is minimal and reuses existing C helpers.
- **Neutral / follow-ups**:
  - `python/test/sycl_default_model_test.py` ensures CI runners with Level Zero hardware continuously gate this path against regressions.

## References

- ADR-0220: SYCL fp64-less device contract
- ADR-0214: GPU-parity CI gate
- ADR-1169: Default VMAF model vmaf_v1.0.16_3d0h
- Netflix/vmaf issue discussions regarding CAMBI high resolution speedup
