---
paths:
  - core/src/feature/cambi.c
  - core/src/feature/cuda/integer_cambi_cuda.c
  - core/src/feature/hip/integer_cambi_hip.c
invariant: CAMBI GPU twins mirror host-side semantics and hybrid host/GPU dispatch contracts.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# CAMBI GPU Twins and Hybrid Host Dispatch Contracts

- **CAMBI GPU twins mirror `cambi.c`'s host-side semantics, not "something
  reasonable" (branch `fix/gpu-cambi-parity-drift`, 2026-09-05)**. `cambi.c` is
  pinned by Netflix golden gate, so when twin and reference disagree
  twin is always side that moves. Two places where natural GPU idiom
  is wrong answer, in both `cuda/integer_cambi/cambi_score.cu` and
  `sycl/integer_cambi_sycl.cpp`:
  - 7×7 zero-derivative box sum must contribute **zero** for taps outside
    image, because `get_spatial_mask_for_index`'s summed-area table
    zero-pads (`compute_dp_row` with `actual_width = 0`). Clamping to edge
    pixel — usual GPU border idiom — inflates sum, because edge pixels
    are `zero_derivative = 1` by construction, and over-marks banding within
    three pixels of every border.
  - vertical `filter_mode` pass must skip `y == 0` and `y == height - 1`.
    `cambi.c::filter_mode` writes back only under `if (i > 1)`, so it fills
    output rows `1 .. height-2` and leaves both border rows at their
    **pre-filter** values. Because V pass writes into buffer H pass
    read from, early return preserves exactly those pixels.

  `cambi_high_res_speedup` (`hrs`) is part of same contract and has three
  separate effects that must all be present in twin: resolution against
  encode pixel count at init, halving adjusted window, and one extra
  decimation before scale 0. default model `vmaf_v1.0.16_3d0h` sets
  `hrs=1080`, so omitting any of them silently changes every `>= 1080p` score.

  **Parity fixtures must have real-content structure.** original
  `test_{cuda,sycl}_cambi_parity.c` fixture was quantised gradient: constant
  down every column and along every border. Both defects above are invisible on
  it, and gates stayed green for months while real content drifted 2.7e-3.
  added "textured" fixture (horizontal bands + vertical ramp + deterministic
  LCG dither + inverted border ring) fails at 2.11e-2 against pre-fix kernel.
  Keep both fixtures; new CAMBI twin needs to pass both.
- **`cambi.c` GPU ports: CUDA / HIP / Metal hybrid host/GPU per
  [ADR-0205](../../../../docs/adr/0205-cambi-gpu-feasibility.md) +
  [ADR-0210](../../../../docs/adr/0210-cambi-vulkan-integration.md); SYCL
  fully on device per
  [ADR-1357](../../../../docs/adr/1357-sycl-cambi-device-resident.md).**
  Hybrid twins offload only embarrassingly-parallel phases (derivative +
  7×7 spatial mask + 2× decimate + 3-tap mode filter) and call
  `vmaf_cambi_calculate_c_values` + `vmaf_cambi_spatial_pooling` on host
  against GPU-produced image + mask; those call sites stay lock-step with
  CPU `calculate_c_values` because they *are* CPU code. `cambi_sycl`
  reimplements c-values (per-chunk column histograms) and top-K pooling
  (radix select + exact 128-bit fixed-point sum) on device: any CPU-side
  change to `c_value_pixel`, histogram window walk,
  `spatial_pooling`, `cambi_preprocessing` or `filter_mode` must be
  mirrored into `sycl/integer_cambi_sycl.cpp` in same PR
  (`test_sycl_cambi_parity` asserts bit-exact per frame). Open host-residual rows
  `T-{CUDA,HIP,METAL}-CAMBI-HOST-RESIDUAL-2026-09-29` in
  `docs/state.md` port SYCL design to other twins.
  - **`cambi_internal.h` invariant**: this internal-only header
    exposes cambi.c's file-static helpers (`get_spatial_mask`,
    `decimate`, `filter_mode`, `calculate_c_values`,
    `spatial_pooling`, `weight_scores_per_scale`,
    `get_pixels_in_window`, `cambi_preprocessing`,
    `increment_range` / `decrement_range` /
    `get_derivative_data_for_row` callbacks) and
    heatmap writers (`open_heatmaps`, `dump_c_values`, close loop:
    `vmaf_cambi_open_heatmaps` / `_dump_c_values` / `_close_heatmaps`,
    called by CPU and by `integer_cambi_metal`), and
    `reciprocal_lut` table (`vmaf_cambi_reciprocal_lut`, ADR-1357 —
    device twins upload it verbatim; 42 entries are 1 ulp off
    `1.0f / i`, so never recompute it) to GPU twins via
    thin trampoline block at bottom of `cambi.c`. **Do not
    rename or change signatures of those helpers without
    updating trampoline block + header in same PR
    or GPU build breaks.** trampoline body is *only*
    fork-added code inside `cambi.c`; upstream-mirror body
    above stays byte-identical to keep Netflix sync clean.
  - Strategy III (fully-on-GPU c-values) from
    [research digest 0020](../../../../docs/research/0020-cambi-gpu-strategies.md)
    is implemented for SYCL by ADR-1357 (column-owned sliding window,
    not direct per-pixel histogram 0020 sketched); see
    [Research-2122](../../../../docs/research/2122-sycl-cambi-device-resident.md).
