<!-- markdownlint-disable MD013 MD060 -->
# ADR-1405: `float_ssim_hip` decimates on the device, bit-identical to the CPU

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: hip, gpu, ssim, performance, numerics, gpu-parity, rc3, fork-local

## Context

CPU `float_ssim` picks a decimation factor from the short side, `max(1, round(min(w, h) / 256))`: 1 below 384 px, 4 at 1920x1080, 8 at 3840x2160. `ssim.c` low-passes both planes with a `scale x scale` box of weight `1.0f / (scale * scale)` and decimates them with `iqa_decimate()` before `iqa_ssim()`. `float_ssim_hip` implemented scale 1 only: its [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md) context check refused every picture with a short side of 384 px or more, so model dispatch and `--backend hip --feature float_ssim` computed the feature on the CPU with the warning "float_ssim_hip cannot run 3840x2160 8-bit pictures with these options", and a direct request failed at init (`T-HIP-FLOAT-SSIM-SCALE-GT1-2026-09-29`). [ADR-1370](1370-sycl-float-ssim-device-decimation.md) solved the same gap for SYCL and proved that the CPU's window sum is exact up to scale 128.

The twin already forms each pixel's SSIM term as the CPU does ([ADR-1382](1382-hip-twin-cpu-option-parity.md)), so with decimated planes equal to the CPU's the only remaining difference is the rounding of the fp32 Gaussian sums. The state row's contract: decimated planes byte-identical to the CPU, `float_ssim` within 5e-5 at the automatic scale, no fp64 in the decimation, one upload and one read-back per frame.

## Decision

`float_ssim_hip` decimates on the device with the CPU's arithmetic, as ADR-1370 does:

1. **A decimation kernel ahead of pass 1.** `calculate_ssim_hip_decimate_{8,16}bpc` reads the raw luma planes the twin already uploads and writes the two fp32 planes `iqa_decimate()` produces. Each output is `iqa_filter_pixel()` at `(x * scale, y * scale)`: offsets `-scale/2 .. scale-1-scale/2`, `KBND_SYMMETRIC` edges, the fp32 product `sample * (1.0f / (scale * scale))`, an int64 sum in units of 2^-52 (exact up to scale 128) and one round-to-nearest conversion to fp32. The tap weight is formed on the host, where the CPU extractor forms it.
2. **One copy of the arithmetic, testable without a device.** The window sum lives in `float_ssim/ssim_decimate.h`, plain C and HIP C++. The kernel compiles it for the device and `test_hip_float_ssim_decimate` compiles the same lines for the host and holds them against `iqa_decimate()` itself.
3. **Scale 1 keeps its path.** Pass 1 reads the raw samples directly at scale 1, as before, and the decimated fp32 planes above it through a third entry point, `calculate_ssim_hip_horiz_f32`; the three pass-1 kernels share one template body. Scale-1 output is unchanged.
4. **The gate refuses only what the device cannot do.** `check_context_hip()` returns `-ENOTSUP` when the decimated plane is smaller than the 11x11 Gaussian or the scale exceeds 128, and 0 otherwise. The plane size comes from the CPU's `iqa_decimate_dim()`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Decimation kernel with an exact int64 window sum, shared with a host test (chosen) | Planes bit-identical to the CPU at every depth and scale 1 to 128; no fp64; the arithmetic is checked on every build, device or not | A new kernel, a third pass-1 entry point and two more device planes above scale 1 | — |
| The CPU's literal `double` sum on the device | Shortest kernel; HIP devices have fp64 | The row's contract excludes fp64 in the decimation; a double sum and the int64 sum are equal only because the sum is exact, which the int64 form states outright | Same result, weaker statement of why |
| Convert every scale through a decimation pass, scale 1 included (the SYCL layout) | One pass-1 kernel | An extra full-resolution pass and two full-resolution fp32 planes at scale 1 for no numerical change | Scale 1 would get slower |
| Decimate on the host and upload the small planes | No new kernel | A full-frame host pass per plane every frame | Violates device residency |
| fp32 accumulation of the window | Simplest kernel | Differs from the CPU for taps that are not a power of two (scales 3, 5, 6, 7, 9, 10); `test_hip_float_ssim_decimate` shows it | Not the CPU's arithmetic |
| Keep scale 1 only and the CPU fallback (ADR-1324) | No change | `float_ssim` never runs on HIP at 1080p or 4K | RC3 asked for the device path |

## Consequences

- **Positive**: `--backend hip --feature float_ssim` and models run `float_ssim_hip` at every size, with no fallback warning. On a gfx1036 (`ryzen-4090-arc`, ROCm 7.2.4) the device's decimated planes equal `iqa_decimate()` byte for byte on 96 planes (BBB 3840x2160 and a 1080p checkerboard at the automatic scale, the Netflix pair at scales 2 to 10 and at 10, 12 and 16 bits, an odd 853x481 clip). `float_ssim` against `--backend cpu` at `--precision max`: 1.79e-6 on BBB 3840x2160 (scale 8), 4.23e-6 on a 1080p crop of it (scale 4), 5.96e-8 and 1.19e-7 on the 1080p checkerboard pairs, 1.79e-7 on the Netflix pair at scales 1 to 10. At 4K a frame takes 5.48 ms on the twin against 11.94 ms for the CPU extractor on 16 threads and 19.68 ms for the previous CPU fallback at the default thread count; at 1080p 1.96 ms against 2.74 ms and 6.56 ms. Scale-1 output is bit-identical to the previous build.
- **Negative**: above scale 1 the twin holds two more device planes of the decimated size. `enable_db` magnifies the linear residual by `4.34 / (1 - ssim)`: 0.018 dB at 30 dB on the 1080p crop.
- **Neutral / follow-ups**: the CUDA and Metal twins stay scale 1 only (`T-CUDA-FLOAT-SSIM-SCALE-GT1-2026-09-29`, `T-METAL-FLOAT-SSIM-SCALE-GT1-2026-09-29`). Guarded by `test_hip_float_ssim_decimate` (device-free, byte identity against `iqa_decimate()`), `test_hip_float_ssim_parity` and its `_large` variant (the SYCL case table at the parity gate's 5e-5, the twin gate at 3840x2160 and below 11x11), `test_gpu_float_ssim_auto_scale_contract.py`, five planted regressions in `test_hip_kernel_source_contract.py` and `test_vmaf_feature_backend_hip`.

## References

- req: RC3 HIP lane brief (2026-10-01): "T-HIP-FLOAT-SSIM-SCALE-GT1-2026-09-29: float_ssim_hip implements scale 1 only, so 1080p and 4K run on the CPU; implement the decimated scales on the device mirroring the CPU bit for bit (ADR-1370 is the SYCL precedent)".
- [ADR-1370](1370-sycl-float-ssim-device-decimation.md) and [Research-2130](../research/2130-sycl-float-ssim-device-decimation.md) — the SYCL design and the exactness proof this port relies on.
- [Research-1405](../research/1405-hip-float-ssim-device-decimation.md) — the plane comparison, parity and timing on the gfx1036.
- [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md) — the context check; [ADR-1359](1359-cli-feature-backend-twin.md) — `--feature` twin routing; [ADR-1382](1382-hip-twin-cpu-option-parity.md) — the per-pixel SSIM term and the options.
