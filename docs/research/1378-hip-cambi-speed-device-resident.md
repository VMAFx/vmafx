<!-- markdownlint-disable MD013 MD060 -->
# Research-1378: HIP CAMBI and SpEED on the device — rounding, replays, what holds without a device

- **Status**: Active
- **Workstream**: RC3 HIP port of `T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29` and `T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`; decisions in [ADR-1378](../adr/1378-hip-cambi-device-resident.md) and [ADR-1384](../adr/1384-hip-speed-device-resident.md)
- **Last updated**: 2026-09-30

## Question

The SYCL twins of CAMBI and SpEED run entirely on the device and match the CPU bit for bit on Intel hardware ([ADR-1357](../adr/1357-sycl-cambi-device-resident.md), [ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md)). What does the same design need on HIP, how is correctly rounded fp32 arithmetic obtained from hipcc, and how much of the parity can be established with no AMD device?

## Sources

- CPU references: `core/src/feature/cambi.c`, `speed.c`, `speed_internal.c`, `vif_tools.c`.
- SYCL designs: `core/src/feature/sycl/integer_cambi_sycl.cpp`, `speed_sycl_pipeline.cpp`, [Research-2122](2122-sycl-cambi-device-resident.md), [Research-1358](1358-sycl-speed-device-resident.md).
- HIP twins: `core/src/feature/hip/integer_cambi_hip.c`, `integer_cambi/cambi_hip_device.h`, `integer_cambi/cambi_score.hip`, `speed_hip_pipeline.{h,c}`, `speed/speed_hip_device.h`, `speed/speed_pipeline.hip`, `speed_chroma_hip.c`, `speed_temporal_hip.c`.
- Toolchain: `vmaf-dev-mcp:ocloc`, HIP 7.15 / AMD clang 23 (`/opt/rocm/lib/llvm/lib/clang/23/include/__clang_hip_math.h`), glibc 2.43, gcc 15.2. Full HIP build for gfx90a, gfx1030, gfx1036 and gfx1100. No AMD GPU: device tests skip (exit 77).

## Findings

### HIP's `__f*_rn` intrinsics are not rounding primitives

In `__clang_hip_math.h`, unless `OCML_BASIC_ROUNDED_OPERATIONS` is defined, `__fadd_rn`, `__fmul_rn` and `__fdiv_rn` are `x + y`, `x * y` and `x / y` (so they contract under hipcc's default `-ffp-contract=fast-honor-pragmas`), and `__fsqrt_rn` is `__ocml_native_sqrt_f32`, an approximation. `sqrtf()` is `__builtin_sqrtf`. The SpEED kernel TU is therefore built with `-ffp-contract=off -fhip-fp32-correctly-rounded-divide-sqrt` and uses plain operators. The device LLVM IR for gfx1036 (`hipcc --cuda-device-only -S -emit-llvm`) has no `contract` flag, no `!fpmath` relaxation and no `double` in any of the eight SpEED kernels; the 142 `fdiv` lower to `v_div_scale` / `v_div_fmas` / `v_div_fixup` sequences and the nine `llvm.sqrt.f32` to `v_sqrt_f32` followed by the FMA residual test of its two neighbouring floats, which picks the correctly rounded one. The 360 `llvm.fmuladd` calls all sit in `speed_hip_scale` and disappear when `sinpif()` is stubbed: they are the inlined OCML `sinpif` of the lanczos4 prescale, which ADR-1358 already leaves at the ADR-0214 tolerance. `fmaf()` stays `llvm.fma.f32`, as the exact two-product needs. The CAMBI kernels carry `contract` only on multiplies whose result is compared or scaled by 2^24, which have no add to fuse with; they have no `fdiv`, square root or `double`.

### glibc's `log2f` is not correctly rounded; the device's is

The device evaluates log2 in fp32 pairs and rounds once (`speed_hd_log2_rn()`, the SYCL and CUDA routine); a sweep of every 61st pattern from 2^-6 to 2^24 matches `(float)log2((double)x)` exactly. glibc 2.43's `log2f` differs from that on 13 665 of 3 595 118 sampled arguments in [1, 8) (0.38 %). The CPU extractor calls the libm `log2f`, so against a glibc build, and only there, a frame whose entropy or weighting hits a misrounded argument differs in the last float bits. Replaying the device chain on the host against the glibc-built CPU extractor, without the test's libm seam, on the Netflix 576x324 pair: `speed_chroma_u` differs on 6 of 48 frames, `_v` on 3, `_uv` on 6, all by at most 4.8e-7; `speed_temporal` is identical on every frame; the option and synthetic cases differ on at most 2 frames each (at most 2.5e-6, weighting mode 5). With a correctly rounded `log2f` preloaded into the CPU run (`float log2f(float x) { return (float)log2((double)x); }`), every frame of every case is identical. The SYCL measurements of ADR-1358 used the icx build, whose libimf `log2f` is correctly rounded on all but 33 of 20 000 000 arguments.

### The replays pin the arithmetic and the parameter block

Both twins keep every per-work-item routine in one header compiled for the device and, with contraction off, for a host test that replays the frame kernel by kernel in launch order (`test_hip_cambi_device_math`, `test_hip_speed_device_math`, `fast` suite). The parameter block comes from the extractors' own planning code (`cambi_hip_plan()`, `speed_hip_params_fill()`, `speed_hip_bindings_*()`), and the init-time configuration from the CPU's helpers, so a planning change cannot pass the replay and fail on the device. The CAMBI replay runs every c-values work-item of a scale in flight at once (begun together, advanced row by row in lockstep) with a canary behind the last histogram, so a work-item writing outside its histogram column is caught. It compares every scale's c-value plane and score with `cambi.c` over 12 fixtures: 8, 9, 10 and 12 bits, a resized encode, anti-dither on and off, the 1080p high-res speed-up, window 127, contrast depths 0 to 5, two chunk layouts, and a full-range ramp that crosses both edges of the level band; all 12 band (no fixture scores 0). It also checks that `cambi_hip`'s `init()` accepts and rejects exactly the windows `cambi.c` does. The SpEED replay compares every frame of the Netflix pair and of synthetic 10-bit (with low-order sample bits), prescale, kernelscale, weighting-mode, `speed_use_ref_diff` and singular-chroma cases against the registered CPU extractors.

Planted regressions, each applied alone and rebuilt, all fail their replay: CAMBI mode filter, level-band edge, anti-dither rounding, c-value by division instead of the table, a tie short in the top-K sum, mask edge handling, the speed-up mask shift and the window pad in the plan; SpEED covariance low parts and double rounding, variance by reciprocal, the Wilkinson shift, the mean subtraction, the 10-bit sample conversion, the regularity epsilon, a truncated log2 series, the anti-alias width in the parameter block and the `speed_use_ref_diff` binding. Three planted changes survived earlier fixtures and were equivalences, not gaps: negating the centred values (SpEED is sign-invariant), `/ 4` versus `* 0.25` (exact for power-of-two scales) and a one-lane covariance group (the pair sum is order-independent); they were replaced by non-equivalent ones.

### On-device verification and timings (`ryzen-4090-arc`, gfx1036)

Kernel launch geometry, barriers, atomics and occupancy were verified on `ryzen-4090-arc` (gfx1036 iGPU, ROCm 7.2.4):

- **Contract and parity tests**: `test_hip_cambi_parity`, `test_hip_cambi_parity_large`, `test_hip_speed_chroma_parity`, `test_hip_speed_temporal_parity`, `test_hip_speed_singular_parity`, `test_hip_speed_chroma_parity_large`, `test_hip_speed_temporal_parity_large` and `test_hip_device_resident_contract` all pass on the device with no skip markers.
- **CAMBI parity**:
  - Netflix 576x324 (48 frames): 48/48 frames identical to CPU (max abs diff 0.0).
  - BBB 3840x2160 (50 frames): 50/50 frames identical to CPU (max abs diff 0.0, well within bound 2.2e-15).
  - Wide short-frame domain (1920x64, 1920x128, 1920x160, 3840x128): all run clean, exit 0, identical to CPU with `T-CAMBI-SHORT-FRAME-OOB-2026-09-30` fixed.
- **SpEED parity**:
  - With exact `log2f` preloaded (`LD_PRELOAD=/tmp/crlog2f.so`): 100% bit-identical on every output (`speed_chroma_u`, `_v`, `_uv`, `speed_temporal`) across all frames on both 576x324 (48/48) and 3840x2160 (50/50), `speed_gpu_parity.py` exit 0.
  - Stock glibc: 47/48 frames (576x324) and 49/50 frames (4K) identical for `speed_chroma_v` (max diff 1.43e-06), identical on all other channels.
  - Short-frame domain: 1920x64 cleanly rejected (chroma dim < 80); 1920x128/160 and 3840x128 run clean and stay in bounds.
- **Timings** (`(median t(22) - median t(2)) / 20` over 3 interleaved runs, before -> after, CPU at 16 threads):
  - CAMBI 576x324: 1.71 -> 1.80 ms/frame (CPU16: 0.14)
  - CAMBI 3840x2160: 91.21 -> 130.55 ms/frame (CPU16: 10.48)
  - `speed_chroma` 576x324: 1.41 -> 0.70 ms/frame (CPU16: 0.07)
  - `speed_chroma` 3840x2160: 22.81 -> 5.55 ms/frame (CPU16: 5.37) — 4.1x speedup
  - `speed_temporal` 576x324: 1.25 -> 0.74 ms/frame (CPU16: 0.40)
  - `speed_temporal` 3840x2160: 46.54 -> 15.15 ms/frame (CPU16: 20.21) — 3.1x speedup

## Alternatives explored

- **Per-operation `__f*_rn` intrinsics** — not rounding primitives on this toolchain (above).
- **Reproducing glibc's `log2f` on the device** — glibc evaluates in fp64; the device path is fp64-free and a libimf build would then differ instead.
- **A HIP emulation runtime to run the kernels on the host** — heavier than a shared per-work-item header and still not the device's scheduling.

## Related

- [ADR-1378](../adr/1378-hip-cambi-device-resident.md), [ADR-1384](../adr/1384-hip-speed-device-resident.md), [ADR-1377](../adr/1377-hip-motion-diff-first.md), [ADR-1357](../adr/1357-sycl-cambi-device-resident.md), [ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md).
- `docs/state.md`: `T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29`, `T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`, `T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19`.
