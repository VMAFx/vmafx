<!-- markdownlint-disable MD013 MD060 -->
# ADR-1384: Run the HIP SpEED twins entirely on the device, in the CPU's fp32 arithmetic

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: hip, gpu, speed, performance, numerics, rc3, fork-local

## Context

`speed_chroma_hip` and `speed_temporal_hip` kept the ADR-0567 split: the host copied and filtered each plane (`picture_copy`, `speed_internal_filter_and_downscale`), the device computed means and covariance, the host read the covariance back and waited on the stream, ran the eigenvalue problem, the regularity decision and the QR factorisation, re-uploaded, and read the entropies back again to score. Every frame round-tripped through the host several times and the scores differed from the CPU in the last bits (`T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`).

[ADR-1358](1358-sycl-speed-device-resident-linalg.md) moved the whole chain onto the device for SYCL and made it bit-identical to the CPU: every stage reproduces `speed.c` operation for operation in fp32, the fp64 expressions of the reference are reproduced with exact fp32 pairs, and division, square root and log2 are correctly rounded. The RC3 brief asks for the same on HIP, with no GPU-CPU round trip, one wait per frame at collect, and one implementation per behaviour (HISS-19). HIP brings its own rounding hazards: hipcc contracts `a * b + c` into FMAs by default (`-ffp-contract=fast-honor-pragmas`), and without `OCML_BASIC_ROUNDED_OPERATIONS` the `__fsqrt_rn()` intrinsic is the native approximate square root while `__fmul_rn()` / `__fadd_rn()` / `__fdiv_rn()` are the plain operators, so they contract too (`__clang_hip_math.h`, ROCm 7.15).

## Decision

1. **The ADR-1358 chain, one pipeline for both twins.** `core/src/feature/hip/speed_hip_pipeline.{h,c}` owns one device arena, pinned staging and result blocks, and a stream (`VmafHipKernelLifecycle`). Per frame the extractor uploads its raw planes through `vmaf_hip_picture_upload_staged()` ([ADR-1377](1377-hip-motion-diff-first.md)) and submits eight kernels (`speed/speed_pipeline.hip`): optional prescale, the anti-alias filter at the 16x-decimated points, mean subtraction with the independent term, the 25 submatrix means, the 25x25 covariance as exact fp32 pairs, eigenvalues + regularity + QR in one work-group per channel, the per-block solve / variance / entropy, and the frame score; then one `SpeedGpuFrameResult` copy. `collect()` is the only wait. `speed_temporal_hip` keeps the previous frame's raw planes on the device in two slots and differences them there.
2. **Exact arithmetic per TU, not per operation.** The SpEED kernel TU is built with `-ffp-contract=off` and `-fhip-fp32-correctly-rounded-divide-sqrt` (`hip_cu_extra_flags` in `core/src/meson.build`), and the device code writes plain `*`, `+`, `/` and `sqrtf()`. Per-operation `__fmul_rn` / `__fadd_rn` / `__fdiv_rn` / `__fsqrt_rn` would be wrong on HIP (see Context). The device LLVM IR carries no `contract` flag, no `!fpmath` relaxation and no `double` in any SpEED kernel; the only `llvm.fmuladd` calls come from `sinpif()` in the lanczos4 prescale path, which ADR-1358 already leaves at the ADR-0214 tolerance because the CPU evaluates those weights in fp64. log2 is `speed_hd_log2_rn()`, fp32-pair evaluated and rounded once, as on SYCL and CUDA.
3. **One header for the kernels and the replay.** Every per-work-item routine is in `speed/speed_hip_device.h`, compiled by hipcc and, with contraction off, by the host compiler for `core/test/test_hip_speed_device_math.c`; the replay builds its parameter block with the pipeline's own `speed_hip_params_fill()`, `speed_hip_taps_fill()` and `speed_hip_bindings_*()`.
4. **One init-time configure for every backend.** The geometry, filter taps and scoring constants come from `speed_internal_gpu_configure()` in `speed_internal.c`, filling the backend-neutral `SpeedGpu*` types of `speed_gpu_common.h`; the SYCL host setup (`speed_sycl_host.cpp`) now calls it too and its types alias the shared ones. The CUDA port carries the identical routine; whichever lands second keeps one copy.
5. **The contract.** Every per-frame `speed_chroma_u/v/uv` and `speed_temporal` equals the CPU extractor bit for bit when the CPU's `log2f` is correctly rounded (libimf, or glibc with a correctly rounded `log2f`); the host replay asserts this on the Netflix pair and synthetic 10-bit, prescale, weighting-mode, `speed_use_ref_diff` and singular fixtures. glibc's `log2f` misrounds 0.38 % of arguments in [1, 8), so against a glibc-built CPU extractor a few chroma frames differ in the last float bits (6 of 48 on the Netflix pair, at most 4.8e-7); `speed_temporal` stays identical there.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-TU `-ffp-contract=off` + correctly rounded `/` and `sqrtf()` (chosen) | Every operation rounds where `speed.c` rounds; plain code the host replay compiles identically; checked in the IR | Relies on a build flag the contract test pins | — |
| `__fmul_rn` / `__fadd_rn` / `__fdiv_rn` / `__fsqrt_rn` per operation | Explicit in the source | On HIP these are the plain operators (contracting) and the native square root unless OCML_BASIC_ROUNDED_OPERATIONS is set; would silently not round correctly | Wrong on this toolchain |
| Keep the host linear algebra, one readback per frame | Smaller change | Still a round trip and a mid-frame wait; the brief rules it out | Brief |
| A HIP-private copy of the SYCL init-time configure | No change outside HIP | A second implementation of the same setup (HISS-19) | HISS-19 |
| Reproduce glibc's `log2f` on the device | Bit-exact against glibc builds | glibc evaluates in double; the device contract is fp64-free, and libimf builds would then differ | The CPU reference differs by libm; the correctly rounded value is the portable target, as on SYCL and CUDA |

## Consequences

- **Positive**: no host stage, one upload, one readback and one wait per frame; the scores reproduce `speed.c` in fp32 (bit-identical against a correctly rounded `log2f`). The replay test (`fast` suite) pins the arithmetic without an AMD device and kills every planted regression of the device header and the parameter block.
- **Negative**: not run on an AMD device in this change; the verify and timing commands, including the correctly rounded `log2f` shim for glibc builds, are in the `T-HIP-SPEED-HOST-RESIDUAL-2026-09-29` row of `docs/state.md`. The eigenvalue sweep runs in one work-group per channel, latency-bound as on SYCL.
- **Neutral / follow-ups**: lanczos4 prescale stays within the ADR-0214 tolerance. `test_hip_device_resident_contract.py` pins the design at the source level: no host stage, staged upload, one readback, the wait in `collect()`, fp64-free device code without inexact intrinsics, and the kernel's exact-arithmetic flags.

## References

- req: RC3 port brief (2026-09-30): "there shouldnt be any gpu cpu rountrips"; SpEED "fully on the device ... bit-exact"; "on HIP use __fmul_rn/__fadd_rn/__fdiv_rn/__fsqrt_rn per operation or -ffp-contract=off per TU (say which and why)".
- [ADR-1358](1358-sycl-speed-device-resident-linalg.md) — the SYCL design ported here; [Research-1358](../research/1358-sycl-speed-device-resident.md).
- [Research-1378](../research/1378-hip-cambi-speed-device-resident.md) — HIP rounding intrinsics, the IR check, the glibc `log2f` measurement and the host replays.
- [ADR-0567](0567-speed-chroma-temporal-real-gpu.md), [ADR-1218](1218-gpu-speed-singular-device-solution.md), [ADR-1377](1377-hip-motion-diff-first.md), [ADR-0214](0214-gpu-parity-ci-gate.md), [ADR-0220](0220-sycl-fp64-fallback.md).
