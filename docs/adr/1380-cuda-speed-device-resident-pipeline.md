<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1380: The CUDA SpEED twins are device-resident, with the 25x25 linear algebra on the device and CPU-exact fp32 arithmetic

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: `cuda`, `speed`, `gpu`, `performance`, `numerics`, `rc3`, `fork-local`

## Context

`speed_chroma_cuda` and `speed_temporal_cuda` kept the
[ADR-0567](0567-speed-chroma-temporal-real-gpu.md) split. Every frame they
copied the input planes to the host, converted and filtered them there
(`picture_copy`, `speed_internal_filter_and_downscale`), uploaded the
decimated planes, read the covariance and independent terms back, ran the
eigenvalue problem, the regularity test, the QR factorisation and the Q^T B
multiply on the host, uploaded the solution, read the per-block entropies and
variances back and combined the score on the host, with a
`cuStreamSynchronize` between the passes. The scores were within the ADR-0214
tolerance of the CPU but not equal to it.

[ADR-1358](1358-sycl-speed-device-resident-linalg.md) moved the whole chain to
the device on SYCL and reproduced `speed.c` operation for operation in fp32.
The maintainer's RC3 requirement, that no GPU twin round-trips through the
host inside a frame, applies to CUDA too
(`T-CUDA-SPEED-HOST-RESIDUAL-2026-09-29`).

## Decision

We port the ADR-1358 chain to CUDA as one pipeline both extractors share:

- `cuda/speed/speed_score.cu` holds nine kernels: `speed_scale_kernel`
  (prescale), `speed_decimate_raw_kernel` / `speed_decimate_scaled_kernel`
  (the anti-alias filter evaluated only at the 16x-decimated points, with the
  picture conversion and the temporal difference folded into the sample
  read), `speed_centre_kernel` (local mean subtraction and the independent
  term), `speed_means_kernel`, `speed_covariance_kernel`,
  `speed_linalg_kernel` (Householder tridiagonalisation, the implicit-shift
  QR sweep, the regularity decision and the Householder QR factorisation, one
  block per channel), `speed_solve_kernel` (Q^T B, back substitution,
  variances and entropies) and `speed_score_kernel`.
- `cuda/speed_cuda_pipeline.c` owns the module, the buffers and the private
  readback stream. Per frame the extractor copies the picture planes the
  engine already uploaded into the pipeline's raw-plane slots, device to
  device (`speed_temporal` keeps the previous frame's luma in the other
  slot), `speed_cuda_pipeline_submit()` enqueues the chain on the reference
  picture's stream and reads the 40-byte `SpeedGpuFrameResult` back on the
  private stream behind an event, and `collect()` waits once. Only the U/V
  combination, the clamp and the collector append run on the host, on those
  scalars.
- Every fp32 operation whose rounding must match the host is written as
  `__fadd_rn`, `__fsub_rn`, `__fmul_rn`, `__fdiv_rn` or `__fsqrt_rn`, which
  nvcc never contracts, and the fatbin is also built with `--fmad=false`.
  `__fdiv_rn` and `__fsqrt_rn` are correctly rounded, so the residual
  refinement SYCL needs for division and square root is not needed here.
  `log2` is evaluated in fp32 pairs and rounded once (libdevice `log2f` is
  not correctly rounded). The pairs carry about 2^-45 of error, which is too
  much for 48 positive floats whose exact `log2` lies closer than that to a
  rounding boundary; both twins look those up in `speed_log2_hard_cases.h`.
  The reference's fp64 steps keep ADR-1358's exact fp32 forms, so no kernel
  uses fp64.
- The per-run constants come from one routine for both GPU backends:
  `speed_internal_gpu_configure()` (`speed_internal.c`) fills the geometry,
  the filter taps and the scoring constants of `speed_gpu_common.h`, which now
  holds the host/device contract types; the SYCL pipeline aliases them and
  `speed_sycl_host.cpp` calls the same routine instead of its own copy
  (HISS-19).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the host linear algebra, one batched readback per frame | Small change | Still a per-frame round trip and host filtering, which is the requirement | Does not meet the requirement |
| Device linear algebra with plain operators and nvcc's default `--fmad=true` | Simplest port | FMA contraction moves eigenvalues and can flip the regular/singular decision; parity no better than the split | Trades exactness for speed |
| Device linear algebra in fp64 | NVIDIA devices have fp64 | The CPU computes in fp32, so fp64 does not reproduce it; 1/64-rate fp64 on consumer GPUs; the HIP port and ADR-0220 want fp64-free kernels | Neither exact nor fast |
| A second, CUDA-only configuration routine | No change to the SYCL host code | Two copies of the geometry, taps and constants that must agree bit for bit | HISS-19 |
| **Port the ADR-1358 chain with round-to-nearest intrinsics (chosen)** | No host compute or mid-frame wait; the same arithmetic as the SYCL twin; one host setup for both backends | About 1 400 lines of device code; the eigenvalue sweep runs on one thread per channel | Chosen |

## Consequences

- **Positive**: per frame the twins do no host-to-device transfer of their
  own, one 40-byte readback and one wait. The device arithmetic is the SYCL
  twin's, which matches the CPU extractor bit for bit where the CPU's
  `log2f` is correctly rounded (see below).
- **Negative**: the CPU extractor evaluates `log2f` with the platform libm.
  glibc 2.43's `log2f` misrounds 0.14 % of the floats in [1, 1024), Intel's
  libimf (the icx build of the `vmaf-dev-mcp` image) 0.00013 %. The device
  rounds correctly, so against a gcc or clang CPU build a few frames differ in
  the last float bits; against an icx build, or a gcc build whose `log2f` is
  replaced by a correctly rounded one, they are identical. The CPU reference
  must also not contract FMAs: icx with `-march=native`, as the image's own
  `/usr/local/bin/vmaf` is built, moves the CPU scores by up to 7.9e-4 at
  1080p. The `lanczos4` prescale is not exact on either device twin: both
  compute the kernel weights in fp32 where the CPU uses fp64 `sin`
  (`T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30`).
- **Measured on an RTX 4090** (`ryzen-4090-arc`, sm_89, driver 615.71.09,
  CUDA 13.4; icx 2026.0 release build without `-march=native`; before =
  `origin/master` `10f27efe2` built the same way; commands and raw output in
  [Research-1379](../research/1379-cuda-cambi-speed-device-resident.md)):
  - `scripts/dev/speed_gpu_parity.py --backend cuda` exits 0: every per-frame
    `speed_chroma_u`, `speed_chroma_v`, `speed_chroma_uv` and `speed_temporal`
    at `--precision max` equals `--backend cpu`, 48/48 on the Netflix 576x324
    pair and 50/50 on BBB 3840x2160. Before, 1 to 10 frames per output
    matched (largest difference 4.0e-5), and `speed_temporal_cuda` failed at
    1920x1080 and 3840x2160: its solve launch asked for
    `((nb + 7) / 8) * 32` threads per block for `nb` 5x5 blocks, past the
    1024-thread limit once `nb` exceeds 256, and `cuLaunchKernel` returned
    `CUDA_ERROR_INVALID_VALUE` (`test_cuda_speed_temporal_parity_1080p` now
    pins 1920x1080). A gcc build of this branch (glibc 2.44)
    differs from its own CPU extractor on 0 to 2 `speed_chroma` frames per
    output, by at most 1.4e-6, and matches on `speed_temporal`.
  - With `speed_prescale=0.5` on BBB 3840x2160, nearest, bilinear and bicubic
    prescale are identical on 6/6 frames; `lanczos4` matches on 1 of 18
    outputs, at most 3.1e-6 relative on that content.
  - `speed_log2()` returns the correctly rounded `log2` of every one of the
    2 139 095 039 positive finite floats on the RTX 4090.
  - `test_cuda_speed_{chroma,temporal,singular}_parity` and the two smoke
    tests pass; `compute-sanitizer` memcheck, racecheck and synccheck report
    no error or hazard on the three parity tests.
  - Per frame on the 576x324 pair, counted with a CUPTI driver-API callback
    over frames 13 to 22: 7 kernel launches, one 40-byte device-to-host copy
    and one stream synchronisation for either twin. Before,
    `speed_chroma_cuda` made 20 device-to-host copies (336 KiB), 16
    host-to-device copies and 6 stream synchronisations.
  - Milliseconds per frame, median of 3 (before -> after; CPU extractor at 16
    threads): `speed_chroma` 24.90 -> 6.89 (CPU 9.11) at 3840x2160 and
    1.51 -> 0.44 (0.12) at 576x324; `speed_temporal` failed -> 5.88 (26.74)
    and 2.81 -> 0.42 (0.92).
- **Neutral / follow-ups**: the HIP twins keep the split
  (`T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`).
  `core/test/test_cuda_device_resident_contract.py` pins the design;
  `test_cuda_speed_{chroma,temporal,singular}_parity` compare one frame's
  score with the CPU extractor's within 1e-4 on a CUDA device and skip
  (exit 77) without one; `scripts/dev/speed_gpu_parity.py` is the
  frame-by-frame, bit-for-bit check.

## References

- `req` (maintainer, 2026-09-29): "there shouldnt be any gpu cpu rountrips".
- [ADR-1358](1358-sycl-speed-device-resident-linalg.md) (the chain ported
  here), [ADR-0567](0567-speed-chroma-temporal-real-gpu.md),
  [ADR-0964](0964-implement-speed-internal-and-wire-gpu-speed-extractors.md),
  [ADR-1202](1202-cuda-speed-chroma-4k-launch-bounds.md),
  [ADR-1218](1218-gpu-speed-singular-device-solution.md),
  [ADR-1336](1336-cuda-context-owned-resource-teardown.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md).
- [Research-1379](../research/1379-cuda-cambi-speed-device-resident.md),
  [Research-1358](../research/1358-sycl-speed-device-resident.md).
