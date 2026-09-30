<!-- markdownlint-disable MD013 MD060 -->
# ADR-1391: The CUDA ssimulacra2 twin is device-resident

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: cuda, gpu, ssimulacra2, performance, numerics, rc3, fork-local

## Context

`ssimulacra2_cuda` ([ADR-0206](0206-ssimulacra2-cuda-sycl.md), tuned by
[ADR-0410](0410-ssimulacra2-cuda-leaks-perf.md) and
[ADR-0456](0456-ssimulacra2-cuda-blur-fusion-transpose.md)) blurred on the device and did
everything else on the host. Per frame it copied the six raw planes of both
device pictures to pinned host memory and waited (`ss2c_stage_raw_planes`),
converted YUV to linear RGB on the CPU, and for each of up to six scales
computed XYB on the CPU, uploaded it, ran the multiply and blur kernels, copied
five three-plane buffers (`mu1`, `mu2`, `s11`, `s22`, `s12`) back, waited, ran
the fp64 SSIM / edge-difference combine and the 2x2 downsample on the CPU. On an
RTX 4090 a 3840x2160 frame took about 0.7 to 0.8 s, several times slower than
the CPU extractor on 16 threads (about 170 ms). The host combine is what made the twin
bit-identical to the CPU.

[ADR-1363](1363-sycl-ssimulacra2-msssim-device-resident.md) moved the SYCL twin
onto the device and opened `T-CUDA-SSIMULACRA2-HOST-COMBINE-2026-09-29` for the
same port on CUDA. The maintainer's RC3 requirement is that there be no GPU/CPU
round trips per frame. Unlike the SYCL device contract
([ADR-0220](0220-sycl-fp64-fallback.md)), every CUDA device the fork supports
has fp64.

## Decision

`ssimulacra2_cuda` becomes a submit/collect extractor whose whole frame runs on
the device in one in-order chain on the picture stream, with no host compute and
no host wait inside the frame:

1. `submit()` makes the stream wait on the distorted picture's ready event
   (a device-side wait) and converts YUV to linear RGB straight from the raw
   planes of both device pictures; the twin makes no copy of its own.
2. Per scale (up to six, stopping before a side drops below 8): XYB; the five
   blurs of `ref`, `dis`, `ref^2`, `dis^2` and `ref*dis` in one horizontal and
   one vertical launch; the per-pixel SSIM and edge-difference terms with six
   sums per channel; the 2x2 downsample of the linear-RGB pyramid.
3. One 864-byte copy of the per-scale sums to pinned memory on the lifecycle
   stream. `collect()` waits once, forms the 108 norms and pools the score with
   the CPU extractor's formulas.

Numerics:

- The device TUs (`ssimulacra2_device.cu`, `ssimulacra2_blur.cu`) build with
  `vmaf_cuda_host_strict_fp_args` plus `--fmad=false`, products that feed an add
  sit in their own expressions, and the YUV matrix uses `__fmaf_rn` in the
  ADR-0891 order. The shared helpers of `ssimulacra2_math.h` and
  `ssimulacra2_score.h` and the EOTF table of `ssimulacra2_eotf_lut.h` compile
  into device code through two new hooks (`VMAF_SS2_FUNC`,
  `VMAF_SS2_EOTF_LUT_STORAGE`; host code keeps the defaults), and the cube root
  divides through `VMAF_SS2_FDIV` = `__fdiv_rn`. YUV conversion, XYB, blurs and
  downsample are therefore bit-identical to `ssimulacra2.c`.
- The per-pixel terms are the CPU's fp64 expressions. Only their summation
  order differs: each thread adds a fixed strided subset, each block reduces in
  a fixed shared-memory tree, then one block per channel reduces the group
  partials. The tree depends only on the plane size, so the score is
  deterministic. Measured: within 1.5e-12 of `--backend cpu` on every tested
  frame (contract 1e-9).
- Blur layout: the horizontal pass gives one warp 32 rows, stages 32-column
  tiles through shared memory with coalesced loads (forming the three products
  on load, one rounding each), walks each row in one lane and stores 32 output
  columns coalesced; the vertical pass walks one column per thread on the
  row-major output, which is coalesced without a transpose. This replaces the
  ADR-0456 layout (separate multiply kernel, one thread per row, transpose,
  vertical pass on the transposed buffer).
- The twin declares an [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md) context
  check: 4:0:0 input (no chroma to convert) and frames below 8x8, which init
  rejects, go to the CPU extractor when the twin was picked by a model or by the
  [ADR-1359](1359-cli-feature-backend-twin.md) `--backend` mapping. `submit()`
  refuses a picture whose geometry or depth differs from init instead of
  reading past its planes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the hybrid, one wait per frame | Bit-identical to the CPU; smallest change | Still copies the planes and five buffers per scale, and the CPU does XYB, combine and downsample; not "no round trips" | Does not meet the requirement |
| Per-pixel terms as exact fp32 pairs (the ADR-1363 SYCL scheme) | Same code shape as SYCL; avoids fp64, which GeForce parts run at 1/64 rate | More code, and pairs only approximate the fp64 terms; CUDA has fp64, so the CPU's own expressions are simpler to audit | Not measured; kept fp64. The combine is the largest kernel at 4K (2.8 of 7.7 ms), so pairs are the first follow-up to measure |
| fp64 `atomicAdd` into the totals | Simplest reduction | Summation order varies run to run, so the score is not reproducible | Rejected: the score must be deterministic |
| Port the ADR-0456 blur layout as is ("v1": separate multiply, one thread per row, transpose, vertical pass on the transposed buffer) | Proven kernels | Measured slower. Median ms per frame, quiet host: 1.09 / 4.44 / 16.17 at 576x324 / 1920x1080 / 3840x2160, against 0.40 / 1.87 / 7.73 for the chosen layout; both bit-identical to each other | Kept the tiled layout |
| **Device-resident chain, fp64 terms, fixed-tree sums, tiled horizontal blur (chosen)** | No host compute or wait mid-frame, one 864-byte readback; deterministic; about 100x faster at 4K | Not bit-identical to the CPU any more (it was); 14.5 full-size three-plane float buffers of device memory (1.4 GB at 4K) | Chosen |

## Consequences

- **Positive**: `ssimulacra2_cuda` per frame on an RTX 4090, before -> after,
  from `scripts/dev/speed_gpu_parity.py` (each run the median of 3 of
  `(t(22) - t(2)) / 20`; the median of three runs shown): 16.86 -> 0.20 ms at
  576x324 and 721.89 -> 7.10 ms at 3840x2160; the same method gives
  238.68 -> 2.15 ms at 1920x1080. The device time of the new chain, from CUPTI,
  is 0.44, 1.99 and 7.70 ms per frame. Every run, the host-load caveat on these
  wall-clock numbers and the per-kernel split are in
  [Research-1391](../research/1391-cuda-ssimulacra2-device-resident.md).
- **Negative**: the twin moves from bit-identical to within about 1e-12 of the
  CPU (1.49e-12 at worst on BBB 3840x2160). It keeps 14.5 full-size three-plane
  float buffers on the device, about 1.4 GB at 4K. 4:0:0 input is rejected at
  init (and routed to the CPU extractor by the context check).
- **Neutral / follow-ups**: the fp64 combine is the largest kernel at 4K
  (36 % of the device time) and the horizontal blur the next (28 %); pairs for
  the combine, a lighter vertical pass that reuses the horizontal-pass buffers
  in place, and fewer launches (36 per frame at six scales) are the tuning
  candidates. The HIP and Metal twins keep their host combine
  (`T-HIP-SSIMULACRA2-HOST-COMBINE-2026-09-29`,
  `T-METAL-SSIMULACRA2-HOST-COMBINE-2026-09-29`).
  `core/test/test_cuda_ssimulacra2_parity.c` now holds every frame to 1e-9 of
  the CPU (was 1e-4 on one frame) and checks the context check;
  `core/test/test_strict_fp_compiler_args.py` asserts the `--fmad=false` entry
  of both device TUs.

## References

- `req` (maintainer, 2026-09-29): "there shouldnt be any gpu cpu rountrips" /
  "remove the host roundtrips and then test again".
- `docs/state.md` row `T-CUDA-SSIMULACRA2-HOST-COMBINE-2026-09-29` (port
  reference and contract).
- [ADR-1363](1363-sycl-ssimulacra2-msssim-device-resident.md) (the SYCL chain
  this ports), [ADR-0206](0206-ssimulacra2-cuda-sycl.md),
  [ADR-0410](0410-ssimulacra2-cuda-leaks-perf.md),
  [ADR-0456](0456-ssimulacra2-cuda-blur-fusion-transpose.md) (the blur layout this
  replaces), [ADR-0891](0891-simd-bit-exact-round2-fmaf-libvmaf-feature-icx.md) and
  [ADR-1205](1205-ssimulacra2-fma-unification-scalar-and-gpu.md) (FMA order),
  [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md),
  [ADR-1336](1336-cuda-context-owned-resource-teardown.md),
  [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md).
- [Research-1391](../research/1391-cuda-ssimulacra2-device-resident.md).
