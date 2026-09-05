# Research: Intel Arc SYCL Default Model Crashes and Parity Fixes

- **Related ADR**: [ADR-1179](../adr/1179-sycl-v1-model-crash-fix.md)
- **Date**: 2026-09-05
- **Scope**: SYCL backend, CAMBI and Speed feature extractors, Intel Arc GPUs

## Problem statement

Running `vmaf --backend sycl --model version=vmaf_v1.0.16_3d0h` (the default model
on master) failed on Intel Arc GPUs with four distinct issues:

1. SIGSEGV in `integer_cambi_sycl.cpp` due to uninitialized bounds and
   under-sized histogram buffers.
2. SIGABRT in `speed_chroma_sycl.cpp` and `speed_temporal_sycl.cpp` due to
   `double` accumulators and `sycl::local_accessor<double, 1>` on Arc GPUs
   without hardware fp64 (`aspect::fp64`), in violation of ADR-0220.
3. Exit 255 / `predict_load_feature_score: -EAGAIN` caused by
   `options_cambi_sycl` lacking the `cambi_high_res_speedup` (`hrs`) option,
   generating mismatched feature dictionary keys.
4. Host SIMD compilation with Intel `icx` exhibited floating-point drift in
   `float_motion` failing Netflix golden assertions unless strict IEEE-754
   precision flags were propagated to SIMD static libraries.

## Root Cause Analysis

### 1. Cambi Initialization & Histogram Buffer Allocation

In `integer_cambi_sycl.cpp`, the original code attempted to dynamically
recalculate TVI and contrast bounds on `init()`, but omitted initializing
`buffers.v_band_base` and `buffers.v_band_size`. Furthermore,
`buffers.histograms` was sized as `num_bins * sizeof(uint32_t)`, whereas
subsequent multi-scale loops access up to `v_band_size`.

Fix:

- Expose `vmaf_cambi_init_tvi_and_vlt` in `cambi_internal.h` and implement it in
  `cambi.c`.
- In `integer_cambi_sycl.cpp::init()`, invoke `vmaf_cambi_init_tvi_and_vlt(s,
  &buffers)` to populate tables identically to the CPU implementation.
- Size histograms with `MAX(num_bins, (unsigned)buffers.v_band_size) *
  sizeof(uint32_t)`.

### 2. FP64 Absence on Intel Arc GPUs (ADR-0220 Violation)

Intel Arc GPUs (ACM-G11 / Alchemist) do not support `sycl::aspect::fp64`.
Both `speed_chroma_sycl.cpp` and `speed_temporal_sycl.cpp` declared `double
accum` and `sycl::local_accessor<double, 1> scr` in their work-group reduction
kernels. When dispatched to an Arc A380, the runtime threw an unhandled
exception terminating the process with SIGABRT.

Fix:

- Replaced all `double` usages in device code, accessors, and host accumulators
  with `float` in both files, strictly conforming to ADR-0220.

### 3. Feature Name Suffix Generation for `cambi_high_res_speedup`

Default model `vmaf_v1.0.16_3d0h` specifies option `cambi_high_res_speedup:
1080` for CAMBI. Libvmaf constructs feature names by scanning options marked
with `VMAF_OPT_FLAG_FEATURE_PARAM`. Because `integer_cambi_sycl.cpp` omitted
this option from `options_cambi_sycl`, the feature name dictionary generated
for SYCL contained `cambi_cmxv_17_vlt_0.06` instead of
`cambi_hrs_1080_cmxv_17_vlt_0.06`. When predicting scores,
`predict_load_feature_score` returned `-EAGAIN` (-11), and `vmaf` aborted with
`problem generating pooled VMAF score`.

Fix:

- Added `cambi_high_res_speedup` with alias `"hrs"` to `options_cambi_sycl`.
- Handled threshold checks (`CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_W/H`), window
  adjustments, and decimation checks (`scale > 0 ||
  s->cambi_high_res_speedup`).

### 4. SIMD Strict-FP under icx

When compiling with `icx`, `x86_avx2_static_lib` and `x86_avx512_static_lib` in
`core/src/meson.build` defaulted to Intel's `-fp-model=fast=1`. This caused a
0.00007 drift on `float_motion` on checkerboard inputs. Adding
`_x86_simd_strict_fp_extra` (`-fp-model=precise`) restored bit-for-bit exact
floating point values matching GCC reference builds.

## Verification

1. Unit tests:
   - `test_integer_cambi_sycl`: 4/4 passed.
   - `test_sycl_cambi_parity`: 2/2 passed.
   - `test_sycl_speed_chroma_parity`: 2/2 passed.
   - `test_sycl_speed_temporal_parity`: 2/2 passed.
2. Regression test:
   - `python/test/sycl_default_model_test.py`: 1/1 passed.
3. Reproducers:
   - 576x324 (48 frames): vmaf CPU=82.816062, SYCL=82.814061 (delta 2.0e-3);
     cambi_hrs_1080 CPU=0.259678, SYCL=0.262341 (delta 2.66e-3 pooled, 7.2e-3
     max per frame — above the 1e-3 verification bound, tracked as
     T-SYCL-CAMBI-PARITY-DRIFT-2026-09-05); integer_adm3 / integer_motion3
     identical at six decimals; speed_chroma_uv delta 2e-6.
   - 1080p 10-px checkerboard: vmaf CPU=0.000000, SYCL=0.000000 (identical at
     `%.6f`; GPU twins are not bit-exact to the CPU).
   - 1080p 1-px checkerboard: vmaf CPU=45.315104, SYCL=45.315104 (identical at
     `%.6f`).
4. Netflix golden gate:
   - `make test-netflix-golden`: 271 passed, 12 skipped, 0 failed.
