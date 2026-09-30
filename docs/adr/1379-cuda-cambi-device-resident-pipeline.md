<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1379: Run the CUDA CAMBI extractor entirely on the device

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: `cuda`, `gpu`, `cambi`, `performance`, `numerics`, `rc3`, `fork-local`

## Context

`cambi_cuda` ([ADR-0360](0360-cambi-cuda.md)) followed the Strategy II hybrid
of [ADR-0205](0205-cambi-gpu-feasibility.md). Per frame it downloaded the
distorted picture and ran `vmaf_cambi_preprocessing` on the host
(`cambi_download_and_preprocess`), uploaded the result again
(`cambi_upload_and_mask`), and at each of the five scales waited on the stream
and copied the image and mask back (`cambi_filter_and_readback`) so the host
could run `vmaf_cambi_calculate_c_values` and `vmaf_cambi_spatial_pooling`
(`cambi_submit_scale`). The GPU idled while the host worked, and frames could
not overlap. The twin also lacked `cambi.c`'s init guard against a window
whose square reaches the reciprocal table: a window above 65 x 65 would have
reached the host `c_value_pixel()` and read past the table.

[ADR-1357](1357-sycl-cambi-device-resident.md) moved the SYCL twin to the
device and made it bit-exact with `cambi.c` whenever the CPU's own top-K
double sum is exact. The maintainer's RC3 requirement, that no GPU twin
round-trips through the host inside a frame, applies to every GPU backend
(`T-CUDA-CAMBI-HOST-RESIDUAL-2026-09-29`).

## Decision

We port the ADR-1357 design to CUDA. `integer_cambi/cambi_score.cu` holds
twelve kernels that `integer_cambi_cuda.c` enqueues on the distorted
picture's stream, the stream its upload ran on:

- `cambi_validate_kernel` (only for a bit depth other than 8 and 16, as
  `validate_image`), `cambi_preprocess_kernel` (10-bit conversion, resize
  through source-index tables computed once at init, anti-dither),
  `cambi_spatial_mask_kernel` (the ADR-0464 shared-memory tile);
- per scale `cambi_decimate_kernel`, `cambi_filter_mode_h_kernel`,
  `cambi_filter_mode_v_levels_kernel` (which also writes the level map),
  `cambi_row_masks_kernel` (run and change bit masks by warp ballot) and
  `cambi_cvals_kernel` (per-chunk column histograms, `c_value_pixel()` float
  for float with `cambi.c`'s reciprocal table, radix pass 0 of the top-K);
- `cambi_radix_hist_kernel`, `cambi_radix_scan_kernel`,
  `cambi_topk_partials_kernel` and `cambi_topk_final_kernel`: a three-pass
  radix select of the k-th largest c-value and the exact top-K sum as a
  128-bit integer in units of 2^-24.

One 88-byte `CambiCudaResults` block comes back per frame on the extractor's
private stream behind an event. `collect()` is the frame's only wait; it
converts the five sums with `vmaf_cambi_fixed_topk_mean()` and weights them
with `vmaf_cambi_weight_scores_per_scale()`. No host code touches pixel,
histogram or c-value data.

Every kernel takes one argument struct by value
([ADR-1215](1215-cuda-psnr-16bpc-plane-argument.md)). The c-value is spelled
`__fmul_rn(__int2float_rn(w * p0 * pm), lut[pm + p0])`, so it does not depend
on `--fmad`.

The host constants come from `cambi.c` itself, so the CPU extractor and both
device-resident twins cannot drift apart (HISS-19). `cambi.c` gains
`vmaf_cambi_check_window_fits_lut()` (its own init guard, now shared),
`vmaf_cambi_adjust_window()`, `vmaf_cambi_mask_index()`,
`vmaf_cambi_resize_source_indices()`, `vmaf_cambi_contrast_weights()` and
`vmaf_cambi_fixed_topk_mean()`; `integer_cambi_sycl.cpp` drops its private
copies of the window, mask-index, resize-walk, weight-table and fixed-point
code and calls the same functions. The CPU extractor's arithmetic does not
change: the guard is the same code and message, moved into a function.

Out of scope, as since ADR-0360: `full_ref` (FR-CAMBI) and the heatmap dump.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the hybrid, batch the five per-scale readbacks into one wait | Small change | Still a per-frame round trip and host c-values, which is the requirement | Does not meet the requirement |
| Device c-values, host top-K (read the c-value plane back) | No device selection code | 33 MB per frame at 4K, a host quick-select, and a wait per scale | Round trip remains |
| Device port of `spatial_pooling()` as written: quick-select, then an in-order fp64 sum | Reproduces the CPU's rounding even when its sum is inexact | Both steps are sequential over the whole plane, so one thread walks up to 8 M values per scale in fp64, which consumer GPUs run at 1/64 rate | Far slower than the host it replaces |
| Float `atomicAdd` of the top-K values | Short | Order-dependent rounding, not reproducible from run to run | Not deterministic |
| **Port the ADR-1357 design: radix select + exact fixed-point sum (chosen)** | No host stage or mid-frame wait; bit-exact whenever the CPU sum is exact; one algorithm on SYCL and CUDA | More device code; the column histograms need `chunks x width x levels` uint16 of scratch (capped at 64 MiB) | Chosen |

## Consequences

- **Positive**: `cambi_cuda` runs every per-frame stage on the device with one
  readback and one wait per frame, so frames pipeline through the engine's
  submit/collect double buffering (`VMAF_FEATURE_DISPATCH_AUTO`). It now
  rejects an oversize window exactly as `cambi.c` does. One set of
  `cambi.c` helpers serves the CPU and both twins.
- **Measured on an RTX 4090** (`ryzen-4090-arc`, sm_89, driver 615.71.09,
  CUDA 13.4; icx 2026.0 release build without `-march=native`; before =
  `origin/master` `10f27efe2` built the same way; commands and raw output in
  [Research-1379](../research/1379-cuda-cambi-speed-device-resident.md)):
  - Every per-frame `cambi` at `--precision max` equals `--backend cpu`: 48/48
    on the Netflix 576x324 pair and 50/50 on BBB 3840x2160.
  - On the wide, short frames of `T-CAMBI-SHORT-FRAME-OOB-2026-09-30`
    (1920x64, 1920x128, 1920x160, 3840x128) the twin equals a CPU build with
    that row's fix on every frame, and `compute-sanitizer --tool memcheck` is
    clean. The hybrid it replaces, which ran `cambi.c`'s c-values walk on the
    host, scored frame 0 differently and then crashed on all four sizes (a
    corrupted picture reference in `close_fex_cuda()`).
  - `test_cuda_cambi_parity` and `test_cuda_cambi_parity_large` pass, and
    `compute-sanitizer` memcheck, racecheck and synccheck report no error or
    hazard on either.
  - Per frame on the 576x324 pair, counted with a CUPTI driver-API callback
    over frames 13 to 22: 65 kernel launches, two memsets, one 88-byte
    device-to-host copy and one stream synchronisation. Before: 19 launches,
    11 device-to-host copies (1.1 MiB: the picture, then the image and mask
    at each of five scales), one upload of the preprocessed picture and 7
    stream synchronisations.
  - Milliseconds per frame, median of 3: 64.71 before and 6.01 after at
    3840x2160, 1.59 and 0.38 at 576x324; the CPU extractor at 16 threads
    takes 19.65 and 0.09.
- **Neutral / follow-ups**: the HIP and Metal twins keep the hybrid
  (`T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29`,
  `T-METAL-CAMBI-HOST-RESIDUAL-2026-09-29`).
  `core/test/test_cuda_device_resident_contract.py` pins the design;
  `test_cuda_cambi_parity` asserts per-frame equality with `cambi.c` on a
  CUDA device and skips (exit 77) without one.

## References

- `req` (maintainer, 2026-09-29): "there shouldnt be any gpu cpu rountrips".
- [ADR-1357](1357-sycl-cambi-device-resident.md) (the design ported here),
  [ADR-0360](0360-cambi-cuda.md), [ADR-0205](0205-cambi-gpu-feasibility.md),
  [ADR-1219](1219-gpu-cambi-tvi-shared-bisection.md),
  [ADR-1215](1215-cuda-psnr-16bpc-plane-argument.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md).
- [Research-1379](../research/1379-cuda-cambi-speed-device-resident.md),
  [Research-2122](../research/2122-sycl-cambi-device-resident.md).
