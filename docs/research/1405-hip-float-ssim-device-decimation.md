<!-- markdownlint-disable MD013 MD060 -->
# Research-1405: float_ssim decimation on the HIP device

- **Status**: Active
- **Workstream**: RC3 HIP lane, `T-HIP-FLOAT-SSIM-SCALE-GT1-2026-09-29`; decision in [ADR-1405](../adr/1405-hip-float-ssim-device-decimation.md)
- **Last updated**: 2026-10-01

## Question

[Research-2130](2130-sycl-float-ssim-device-decimation.md) showed that the CPU's `float_ssim` decimation is exact up to scale 128 and can be reproduced on a device without fp64. Does the same arithmetic give the CPU's planes on an AMD device, where the int64-to-fp32 conversion is the compiler's own, and what does `float_ssim_hip` then cost and deviate at 1080p and 4K?

## Sources

- `core/src/feature/ssim.c`, `core/src/feature/iqa/decimate.c`, `core/src/feature/iqa/convolve.c` (`iqa_filter_pixel()`, `KBND_SYMMETRIC`), `core/src/feature/picture_copy.cpp`.
- `core/src/feature/hip/float_ssim/ssim_decimate.h`, `core/src/feature/hip/float_ssim/ssim_score.hip`, `core/src/feature/hip/float_ssim_hip.c`.
- Host: `ryzen-4090-arc`, AMD gfx1036 iGPU, ROCm 7.2.4, Linux 7.2.8; `meson setup build-hip core -Denable_hip=true -Denable_hipcc=true -Dhip_gfx_targets=gfx1036 -Denable_cuda=false -Denable_sycl=false --buildtype=release -Db_lto=false`.

## Findings

### The host compiles the kernel's arithmetic and it equals `iqa_decimate()`

`ssim_decimate.h` is plain C as well as HIP C++. `test_hip_float_ssim_decimate` builds the CPU plane with `picture_copy()` and `iqa_decimate()` (in place, `ssim.c`'s kernel) and the device plane with `vmaf_hip_ssim_decimate_sample()` from the raw samples, and compares bytes: scales 1 to 10 on 576x324, 10 / 12 / 16 bits at scales 2 to 10, odd sizes (853x481, 321x181, 33x17, 1x1), scale 128 (1410x1410 and 16-bit 1281x300) and windows wider than the plane (5x3 at scale 16, 7x3 at scale 9). All equal. An fp32 running sum of the same products differs from the CPU on a 576x324 plane at scale 3, so the comparison can tell the two apart.

### The device produces the same bytes

A dev-only build copied `d_ref_dec` / `d_cmp_dec` back in `collect()` and wrote them to disk; a C program linked against `libvmaf.a` ran `picture_copy()` and `iqa_decimate()` on the same frames; `cmp` compared the files. 96 planes (two frames, both pictures, 24 cases), all byte-identical:

| Input | Scale | Decimated |
|---|---|---|
| BBB 3840x2160 8-bit | auto (8) | 480x270 |
| Checkerboard 1920x1080 8-bit | auto (4) | 480x270 |
| Netflix 576x324 8-bit | 2 to 10 | 288x162 .. 57x32 |
| Netflix 576x324 10-bit | 3, 5, 6 | 192x108, 115x64, 96x54 |
| Netflix 576x324 12-bit | 3, 5, 10 | 192x108, 115x64, 57x32 |
| Netflix 576x324 16-bit | 3, 7, 10 | 192x108, 82x46, 57x32 |
| Noise 853x481 4:4:4 8-bit (odd both) | auto (2), 3, 6, 9 | 427x241, 285x161, 143x81, 95x54 |

So `(float)int64` on the gfx1036 rounds to nearest even, as the host's does.

### Score parity against `--backend cpu`, `--precision max` (max abs difference over all frames)

| Fixture | Option | `float_ssim` | extra |
|---|---|---|---|
| Netflix 576x324 (48 fr) | auto (1) | 1.79e-7 | 0 of 48 identical |
| Netflix 576x324 | `scale=2` / `3` / `5` / `10` | 5.96e-8 / 5.96e-8 / 5.96e-8 / 1.79e-7 | 28 / 35 / 23 / 18 of 48 identical |
| Netflix 576x324 | `scale=3:enable_lcs:enable_db:clip_db` | 8.46e-6 dB | l identical, c / s 5.96e-8 |
| Netflix 576x324 10-bit (3 fr) | auto (1) | 1.19e-7 | — |
| Checkerboard 1920x1080 1 px / 10 px (3 fr) | auto (4) | 5.96e-8 / 1.19e-7 | 2 / 0 of 3 identical |
| BBB 1920x1080 crop (24 fr) | auto (4) | 4.23e-6 | — |
| BBB 1920x1080 crop | `scale=2` / `scale=3` | 8.05e-6 / 8.64e-6 | — |
| BBB 1920x1080 crop | `enable_lcs` | 4.23e-6 | l identical, c 7.15e-7, s 4.41e-6 |
| BBB 1920x1080 crop | `enable_db:clip_db` | 0.018 dB (at ~30 dB) | — |
| BBB 3840x2160 (50 fr) | auto (8) | 1.79e-6 | — |
| BBB 3840x2160 | `enable_lcs` | 1.79e-6 | l identical, c 3.58e-7, s 2.09e-6 |
| BBB 3840x2160 | `enable_db:clip_db` | 0.0082 dB (at ~30 dB) | — |

Everything is inside the 5e-5 `float_ssim` tolerance of `scripts/ci/cross_backend_parity_gate.py`. The residual is the SSIM stage's fp32 Gaussian sums (hipcc contracts them into FMAs; `T-HIP-FP-CONTRACT-DEFAULT-2026-09-29`), not the decimation. The 1080p crop is the 1920x1080 window at (960, 540) of the first 24 BBB 4K frames.

At scale 1 the twin is bit-identical to the build before the change: Netflix 8-bit (48 of 48) and 10-bit, and explicit `scale=1` on the 1080p checkerboard and BBB 4K, with and without `enable_lcs` / `enable_db`.

### Time per frame

`(t(22) - t(2)) / 20`, median of 5 repetitions, the whole `vmaf` CLI (YUV read included), load average 13 to 17 from other jobs on the host:

| Configuration | 3840x2160 | 1920x1080 |
|---|---|---|
| `float_ssim_hip` (auto scale 8 / 4) | 5.48 ms | 1.96 ms |
| CPU `float_ssim`, `--threads 16` | 11.94 ms | 2.74 ms |
| Before: `--backend hip --feature float_ssim` fell back to the CPU, default threads | 19.68 ms | 6.56 ms |
| For scale: `psnr_hip` (uploads all three planes of both pictures) | 7.72 ms | 1.81 ms |

The five samples per configuration were within 0.6 ms of their median at 4K. Explicit `scale=1` at 4K, a full-resolution SSIM, takes 88.8 ms after and 93.2 ms before (median of 3, a noisier run at load average 15 to 47), unchanged within noise.

## Alternatives explored

See the decision matrix in [ADR-1405](../adr/1405-hip-float-ssim-device-decimation.md).

## Open questions

- The CUDA and Metal twins still implement scale 1 only.
- With contraction off in the SSIM passes (`T-HIP-FP-CONTRACT-DEFAULT-2026-09-29`) the residual above should shrink further; not measured here.

## Related

- [ADR-1405](../adr/1405-hip-float-ssim-device-decimation.md), [ADR-1370](../adr/1370-sycl-float-ssim-device-decimation.md), [Research-2130](2130-sycl-float-ssim-device-decimation.md), [ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md), [ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md).
