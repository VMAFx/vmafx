<!-- markdownlint-disable MD013 MD060 -->
# Research-2130: float_ssim decimation on the SYCL device

- **Status**: Active
- **Workstream**: [ADR-1370](../adr/1370-sycl-float-ssim-device-decimation.md)
- **Last updated**: 2026-09-29

## Question

CPU `float_ssim` decimates both planes by `scale = max(1, round(min(w, h) / 256))` before SSIM (4 at 1080p, 8 at 4K); `float_ssim_sycl` handled scale 1 only and fell back to the CPU above it. Can the device reproduce the CPU's decimated planes exactly, without fp64 and without a host pass, and what does the twin then cost and deviate at 1080p and 4K?

## Sources

- `core/src/feature/ssim.c` — `compute_ssim()`, `ssim_low_pass_alloc()` (tap `1.0f / (scale * scale)`, `KBND_SYMMETRIC`), `ssim_decimate_pair()` (in place, both planes).
- `core/src/feature/iqa/decimate.c` — `iqa_decimate()`: output `(x, y)` = `iqa_filter_pixel(img, w, h, x * factor, y * factor, k, 1.0f)`, size `w / factor + (w & 1)`.
- `core/src/feature/iqa/convolve.c` — `iqa_filter_pixel()`: offsets `-k/2 .. k/2 - (k even)`, `const float prod = img * kernel; sum += (double)prod;`, returns `(float)(sum * kscale)`; `KBND_SYMMETRIC` (period `2n` mirror).
- `core/src/feature/picture_copy.cpp` — 8-bit samples as is; 10 / 12 / 16 bits as uint16 divided by 4 / 16 / 256.
- `core/src/feature/iqa/ssim_tools.c` — `iqa_ssim()` returns the frame means as fp32.
- [Research-0985](0985-sycl-parity-divergence-2026-06-03.md) — the twin's combined-formula SSIM stage; [Research-2127](2127-sycl-twin-cpu-option-parity.md) — the options.

## Findings

### The CPU sum is exact, so the device can match it without fp64

Samples are multiples of 2^-8 (16-bit `picture_copy()` is the finest). For `scale <= 128` the tap `fl(1 / scale^2)` is at least 2^-14, so a non-zero fp32 product `fl(sample * tap)` is at least 2^-22, and its ulp, and every partial sum, is a multiple of 2^-45. A window sums below 256 (`scale^2 * fl(255.996 * fl(1 / scale^2)) < 256`), so every partial sum fits in 53 bits: the CPU's `double` sum is exact, and `(float)sum` is one round-to-nearest-even of the exact value. The device reproduces it with `int64` in units of 2^-52 (`product * 2^52` is an exact integer below 2^60, the sum below 2^63) and one `convert<float, rounding_mode::rte>` followed by an exact `* 2^-52`. For power-of-two scales (2, 4, 8) the products are themselves exact; the fixed point matters for 3, 5, 6, 7, 9 and 10, where `fl(1 / scale^2)` rounds. Above scale 128 (a short side of 32 896 px or more) the CPU sum stops being exact, so the context check refuses.

`iqa_decimate()` writes its output into the plane it reads (`ssim_decimate_pair()` passes no result buffer). Output row `y` is stored at linear `[y * sw, (y + 1) * sw)`, about row `y / scale` of the source, while every later output reads rows `>= y * scale - scale / 2`; within row 0 the written columns `0 .. x - 1` stay left of the next window. No read sees a written value, so decimating from an untouched copy (the device) gives the same planes. The harness below confirms it on every case.

### Bit identity of the decimated planes (Arc B580 and UHD 770)

A dev-only build dumped `d_ref` / `d_cmp` after `launch_decimate()`; a C program linked against `libvmaf.a` ran the CPU `iqa_decimate()` with `ssim.c`'s kernel on the same frame; `cmp` compared the files. All 50 planes per device were byte-identical, every scale from 1 to 10 among them:

| Input | Scale | Decimated |
|---|---|---|
| BBB 3840x2160 8-bit | auto (8) | 480x270 |
| BBB 1920x1080 8-bit | auto (4) | 480x270 |
| Netflix 576x324 8-bit | 1 to 10 | 576x324 .. 57x32 |
| Netflix 576x324 10-bit | 3, 5, 6 | 192x108, 115x64, 96x54 |
| Netflix 576x324 12-bit | 3, 5, 10 | 192x108, 115x64, 57x32 |
| Netflix 576x324 16-bit | 3, 7, 10 | 192x108, 82x46, 57x32 |
| BBB 853x481 4:4:4 8-bit (odd both) | auto (2), 3, 6, 9 | 427x241, 285x161, 143x81, 95x54 |

### Score parity against `--backend cpu` (max abs difference over all frames)

Identical on the B580 and the UHD 770 in every case. The residual is the twin's fp32 SSIM stage, not the decimation:

| Fixture | Option | `float_ssim` | extra |
|---|---|---|---|
| Netflix 576x324 (48 fr) | auto (1) | 2.98e-7 | — |
| Netflix 576x324 | `scale=2` / `scale=3` | 5.96e-8 / 5.96e-8 | — |
| Netflix 576x324 10-bit (3 fr) | `scale=3` | 5.96e-8 | — |
| Netflix 576x324 | `scale=3:enable_lcs:enable_db:clip_db` | 9.72e-6 dB | l / c / s 5.96e-8 |
| BBB 1920x1080 (24 fr) | auto (4) | 4.29e-5 | — |
| BBB 1920x1080 | `scale=2` / `scale=3` | 5.64e-5 / 3.75e-5 | — |
| BBB 1920x1080 | `scale=1` (unchanged path) | 7.78e-5 | — |
| BBB 3840x2160 (24 fr) | auto (8) | 4.30e-5 | — |
| BBB 3840x2160 | `enable_lcs` | 4.30e-5 | l 5.96e-8, c 4.17e-7, s 1.25e-6 |
| BBB 3840x2160 | `enable_db` (+ `clip_db`) | 0.22 dB (at ~30 dB) | frames 0-3: 121 = 121 |
| BBB 853x480 4:4:4 (24 fr) | auto (2) / `scale=3` | 4.46e-5 / 4.79e-5 | — |

The auto scales, which is what a model or a bare `--feature float_ssim` uses, stay inside the 5e-5 `float_ssim` tolerance of `scripts/ci/cross_backend_parity_gate.py`. Explicit `scale=1` / `scale=2` on BBB 1080p exceed it; scale 1 is the previous twin's output byte for byte, so this predates the change. At scale 1 the new twin is byte-identical to the previous one on the Netflix pair (with and without `enable_lcs` / `enable_db`) and on BBB 1080p, before the fp32 mean rounding below.

### A double frame mean breaks `enable_db` near 1

The first four BBB 4K frames score `1 - 4.4e-10` on the device and exactly 1 on the CPU, because `iqa_ssim()` returns `(float)(sum / n)`. With `enable_db:clip_db` the device reported 93.6 dB against the CPU's 121 dB ceiling (27.4 dB apart). Rounding the twin's means to fp32 the same way makes those frames 121 dB; the largest remaining dB gap is the linear residual magnified by `4.34 / (1 - ssim)` (0.22 dB at 30 dB). The rounding moves a linear score by at most half an fp32 ulp of the mean.

### The combined-formula SSIM stage dominates the residual

Replacing the headline per-pixel value by the product of the `enable_lcs` kernel's clamped L, C and S terms (CPU `ssim_tools.c` decomposition) in a dev build gave 1.8e-7 (Netflix), 4.8e-7 (BBB 1080p `scale=1`), 6.6e-7 (BBB 1080p auto), 8.9e-7 (BBB 4K) and 5.4e-7 (853x480): 50-160x closer to the CPU. It is not in this change: identical windows must stay exactly 1 (ADR-1365), and with an approximate device `sqrt` / division `sqrt(v * v)` and `x / x` are not guaranteed to be exact, so each of L, C and S needs the equal-operand guard the combined formula has. Tracked in `docs/state.md`.

### Time per frame at 4K (BBB 3840x2160 8-bit, auto scale 8)

`(t(22) - t(2)) / 20`, median of 3 repetitions, each series inside `flock /f/gpu.lock`, load average 1.1-1.4, the whole `vmaf` CLI (YUV read included):

| Configuration | ms / frame |
|---|---|
| CPU `float_ssim`, `--threads 16` (i9-12900K) | 10.97 |
| `float_ssim_sycl`, Arc B580 | 6.70 |
| `float_ssim_sycl`, UHD 770 | 12.03 |
| Before: `--backend sycl` fell back to CPU, default threads | 29.32 |
| Before: same fallback, `--threads 16` | 10.55 |
| CLI floor: `psnr_sycl`, Arc B580 | 6.09 |
| `scale=1` (full-resolution SSIM), B580 after / before | 7.81 / 10.55 |
| `scale=1`, UHD 770 after / before | 47.70 / 47.17 |

On the B580 the twin costs 0.6 ms per frame above the CLI's own floor; the upload is now one byte per 8-bit sample instead of four. On the UHD 770 the shared-memory iGPU is slower than 16 CPU threads, and at scale 1 its full-resolution SSIM stage dominates, so the upload change does not show there.

## Alternatives explored

- fp32 window sums: differ from the CPU for non-power-of-two taps; rejected.
- float-float (two-sum) sums: not provably the CPU's final rounding for up to 100 terms; rejected.
- Host decimation and upload of the small planes: keeps a full-frame host pass; rejected by the no-round-trip requirement.
- Reading the shared graph frame instead of an own upload: would share one upload with the VMAF model's other SYCL features, but changes the ADR-0458 self-contained posture; left as a follow-up.

## Open questions

- The CUDA, HIP and Metal twins still implement scale 1 only; not measured here (no device).
- Whether `vmaf_read_pictures_sycl()` (device buffers only) should feed this twin from the shared frame; it now fails `submit()` cleanly instead of dereferencing NULL.

## Related

- [ADR-1370](../adr/1370-sycl-float-ssim-device-decimation.md), [ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md), [ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md), [ADR-1359](../adr/1359-cli-feature-backend-twin.md).
- Tests: `core/test/test_sycl_float_ssim_parity.c` (+ `_large`), `core/test/test_feature_backend_twin.c`, `core/test/test_gpu_float_ssim_auto_scale_contract.py`, `core/tools/test/test_vmaf_feature_backend.sh`.
