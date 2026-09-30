<!-- markdownlint-disable MD013 MD060 -->
# ADR-1390: The HIP ssimulacra2 twin is device-resident with tiled row-pass Gaussian blur

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: hip, gpu, ssimulacra2, performance, numerics, rc3, fork-local

## Context

`ssimulacra2_hip` previously executed only the 3-pole recursive Gaussian blur on
the GPU while performing all intermediate stage compute and image downsampling on
the host. Per frame, it converted input YUV planes to linear RGB on the CPU, and
for each of the 6 pyramid scales:

1. Computed XYB representation on the host.
2. Uploaded both reference and distorted XYB images to the device.
3. Enqueued 5 blurs (three planes each).
4. Downloaded 5 full-size three-plane float buffers (`mu1`, `mu2`, `s11`, `s22`,
   `s12`) back to the host and synchronized the HIP stream.
5. Computed the per-pixel SSIM and edge-difference combine on the host in `double`.
6. Downsampled 2x2 on the host for the next scale.

At 3840x2160, this round-trip design copied approximately 4 GB of image buffers
across the PCIe / memory bus per frame and introduced 6 blocking stream synchronizations.

Following the SYCL twin's port in [ADR-1363](1363-sycl-ssimulacra2-msssim-device-resident.md),
the maintainer requirement for RC3 is that all per-scale GPU/CPU round trips and
mid-frame stream synchronizations be eliminated ("there shouldnt be any gpu cpu
rountrips").

## Decision

**ssimulacra2_hip** is restructured as a fully device-resident submit/collect
pipeline implemented in `core/src/feature/hip/ssimulacra2_hip.c` and
`core/src/feature/hip/ssimulacra2/ssimulacra2_device.hip`:

1. **Single upload, single readback**: `submit()` stages the raw Y/U/V planes of
   both reference and distorted pictures into pinned host staging buffers and
   enqueues asynchronous H2D copies onto the extractor's private HIP stream.
   The entire pipeline—YUV-to-linear RGB conversion, XYB conversion, 3 products,
   5 recursive Gaussian blurs, SSIM and edge-difference accumulation, and 2x2
   pyramid downsampling across all 6 scales—runs entirely on the GPU.
   A single 864-byte readback of per-scale totals is enqueued at the end of the
   frame. `collect()` synchronizes once per frame to read back the sums,
   forming the 108 norms and the final pooled score in fp64 using CPU-matching
   formulas.

2. **Numerics & contraction control**: The device translation unit
   `ssimulacra2_device.hip` is compiled with `-ffp-contract=off` in
   `core/src/meson.build` (`hip_cu_extra_flags`) and sets `#pragma clang fp contract(off)`.
   Single-rounded `fmaf()` is used strictly in the YUV matrix (ADR-0891 / ADR-1205),
   and the shared cube root in `feature/ssimulacra2_math.h` evaluates division
   via `__fdiv_rn()` (`VMAF_SS2_FDIV`).

3. **Deterministic exact fp32 pair summation**: Because the GPU lacks hardware
   fp64, per-pixel terms that the CPU evaluates in `double`
   (`1 - num_m * num_s / denom_s`, edge ratios, fourth powers) are evaluated
   using exact fp32 pairs (`Ff`) with error $\approx 2^{-44}$. The terms are
   accumulated in a deterministic fixed tree:
   - Strided subset per work-item.
   - LDS (shared memory) work-group reduction tree (`ss2h_group_tree`).
   - Final channel reduction pass (`ssimulacra2_combine_final`).
   The summation order depends strictly on plane dimensions, ensuring bit-identical
   results across different GPU wavefront architectures (Wave32 vs Wave64).

4. **Tiled shared-memory row blur**: On AMD RDNA architectures (such as gfx1036),
   a naive row pass where one lane processes one entire row in global memory
   causes uncoalesced memory accesses, taking 322 ms per 4K launch. The row
   pass is optimized using tiled shared memory (`ss2h_blur_rows` with
   `SS2H_ROW_TILE` rows, single-wave blocks, two-slot ring, and register prefetch).
   Consecutive lanes load consecutive columns into shared memory, walk their
   rows in registers, and store coalesced tiles back.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep host combine, batch uploads | Smallest diff, bit-identical to CPU | ~4 GB transfer per 4K frame, host compute bottleneck | Fails RC3 "no host round-trips" requirement |
| Device combine in plain fp32 | Simplest device code | Numerical cancellation and non-deterministic float add reordering | Violates 1e-9 tolerance and reproducibility |
| Uncoalesced global row pass | Simpler kernel | 42x slower row blur pass on gfx1036 (322 ms vs 7.7 ms per 4K launch) | Bottlenecks GPU pipeline on memory latency |
| **Device-resident chain with tiled row pass and exact pairs (chosen)** | Zero mid-frame host round-trips; deterministic; < 1e-11 diff vs CPU; 5x faster on gfx1036 | Non-bit-identical to CPU fp64 sequential sum; specialized LDS tiling | Chosen |

## Consequences

- **Positive**: Eliminates all mid-frame host round-trips and stream synchronizations.
  On AMD gfx1036:
  - 576x324: 5.68 ms/frame (CPU 16t: 2.89 ms/frame).
  - 3840x2160 (4K): 234.30 ms/frame (CPU 16t: 148.03 ms/frame), a >5x speedup over the previous host-roundtrip implementation.
  - Parity vs CPU: max abs diff `1.123e-12` on 576x324 (48 frames) and `5.826e-13` on 4K (50 frames), both well within the `1e-9` tolerance.
- **Negative**: No longer bit-identical to the sequential CPU double sum due to tree reduction order.
- **Verification**: `python3 scripts/dev/speed_gpu_parity.py --backend hip --feature ssimulacra2 --max-abs-diff 1e-9`, `test_hip_ssimulacra2_parity`, `test_hip_ssimulacra2_parity_large`, `test_hip_ssimulacra2_init_unwind`.

## References

- [ADR-1363](1363-sycl-ssimulacra2-msssim-device-resident.md) (SYCL ssimulacra2 device-resident chain)
- [ADR-0206](0206-ssimulacra2-cuda-sycl.md) (Initial ssimulacra2 GPU scaffold)
- [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) (RC sequence)
