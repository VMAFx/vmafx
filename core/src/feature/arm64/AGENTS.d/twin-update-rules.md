---
paths:
  - core/src/feature/arm64/*.c
  - core/src/feature/x86/*.c
  - core/src/feature/iqa/convolve.c
  - core/src/feature/iqa/ssim_tools.c
  - core/src/feature/iqa/ssim_accumulate_lane.h
  - core/src/feature/ms_ssim_decimate.c
  - core/src/feature/third_party/xiph/psnr_hvs.c
  - core/src/feature/ssimulacra2.c
  - core/src/feature/cambi.c
  - core/src/feature/cambi_c_values_frame.h
  - core/src/feature/moment.c
  - core/test/test_psnr_hvs_dispatch_invariance.c
  - core/test/test_cambi_stage_simd.c
  - core/test/test_cambi_spatial_mask_simd.c
  - core/test/test_cambi_dispatch_invariance.c
  - core/test/test_cambi_simd.c
  - core/test/test_moment_simd.c
invariant: Lockstep twin-bundle updates across ARM64 NEON/SVE2, x86 AVX2/AVX-512, and scalar references.
---
# ARM64 SIMD twin-update bundles

## Twin-update rules

TUs come in twin-bundles. Change to one half **must** ship with matching change
to other halves in **same PR**:

| Group | TUs that move in lockstep |
| --- | --- |
| **SSIM accumulate** (ADR-0139) | `ssim_neon.c` + `../x86/ssim_avx2.c` + `../x86/ssim_avx512.c` + scalar `../iqa/ssim_tools.c` + shared helper `../iqa/ssim_accumulate_lane.h` |
| **IQA convolve** (ADR-0138 + ADR-0143) | `convolve_neon.c` + `../x86/convolve_avx2.c` + `../x86/convolve_avx512.c` + scalar `../iqa/convolve.c` |
| **MS-SSIM decimate LPF** (ADR-0125) | `ms_ssim_decimate_neon.c` + `../x86/ms_ssim_decimate_avx2.c` + `../x86/ms_ssim_decimate_avx512.c` + scalar `../ms_ssim_decimate.c`. 9-tap filter table appears verbatim in all four. |
| **PSNR-HVS** (ADR-0160, [ADR-1469](../../../../../docs/adr/1469-psnr-hvs-simd-butterfly-two-stages.md)) | `psnr_hvs_neon.c` + `../x86/psnr_hvs_avx2.c` + scalar `../third_party/xiph/psnr_hvs.c`. Butterfly: 34 scalar statements, same text / order / names. Both SIMD twins: two functions, `od_bin_fdct8_even_simd()` (21 statements) + `od_bin_fdct8_odd_simd()` (13), cut at same statement; move cut in both or neither. Threading `ret` by pointer: load-bearing. `compute_masks()`: keep scalar `sqrt(mask * gvar) / 32` with `float` product (upstream's statement, ADR-1488); `double` product = one ulp off on ~1 block in 20. Gate: `test_psnr_hvs_dispatch_invariance` (whole path vs shipped scalar, bit compare, x86-64 + aarch64); run under `qemu-aarch64` after touching any of three. |
| **SSIMULACRA 2 SIMD** (ADR-0161 / 0162 / 0163 / 0213 / 0252) | `ssimulacra2_neon.c` + `ssimulacra2_sve2.c` + `../x86/ssimulacra2_avx2.c` + `../x86/ssimulacra2_avx512.c` + `ssimulacra2_host_neon.c` + `../x86/ssimulacra2_host_avx2.c` + scalar `../ssimulacra2.c` + Vulkan host-path `../vulkan/ssimulacra2_vulkan.c` |
| **CAMBI stage kernels** (ADR-1256, Research-2065) | `cambi_neon.c` + `../x86/cambi_avx2.c` (upstream mirror) + `../x86/cambi_avx512.c` + scalar `../cambi.c`; NEON / AVX-512 c-values drivers share walk `../cambi_c_values_frame.h`. Dispatched on NEON: anti-dither, derivative, decimate, dp row, c-values. Kept scalar: mask row, mode filter. Tests: `test_cambi_stage_simd.c`, `test_cambi_dispatch_invariance.c`, `test_cambi_simd.c` (run under `qemu-aarch64` without aarch64 host). |
| **CAMBI spatial-mask rows** (ADR-1256) | `cambi_neon.c` (`compute_dp_row_neon`, `compute_mask_row_neon`) + `../x86/cambi_avx2.c` + `../x86/cambi_avx512.c` twins + scalar reference in `../cambi.c`. Only dp row is dispatched on aarch64: GCC and Clang auto-vectorize scalar mask row into same `cmhi` / `uzp1` sequence, so `compute_mask_row_neon` stays built, parity-tested and undispatched — re-check compiled scalar before wiring it. dp row keeps single add on loop-carried chain; keep that shape. Tested in `../../test/test_cambi_spatial_mask_simd.c` (run under `qemu-aarch64` when no aarch64 host is available). |
| **float_moment SIMD** ([ADR-1500](../../../../../docs/adr/1500-arm-float-moment-scalar-order.md)) | `moment_neon.c` + `moment_sve2.c` + `../x86/moment_avx2.c` + `../x86/moment_avx512.c` + scalar `../moment.c`. Every kernel squares in `float` (second moment) and adds each value into one `double` in raster order, one after other: scalar's bits on every input and SVE vector length. No lane accumulators, no per-row vector sums, no `svaddv` / `vaddvq_f64` reduction: past 2^53 units sum rounds on every add and any other grouping is another number. Gate: `test_moment_simd` (`==`) under `qemu-aarch64` with `sve=off`, `sve128`, `sve256`, `sve512` and `sve2048` after touching any of them. |
| **Motion v2 NEON** (ADR-0145) | `motion_v2_neon.c` uses **arithmetic** right-shift (`vshrq_n_s64(v, 16)` / `vshlq_s64(v, -(int64_t)bpc)`); matches scalar. Sister `../x86/motion_v2_avx2.c` uses `_mm256_srlv_epi64` (logical) — knowingly out-of-spec until AVX2 audit. **Do NOT port AVX2 logical pattern here.** 4-lane stride + scalar tails on both sides of row are load-bearing for x_conv edge-mirror contract. |

Complete invariants in [../AGENTS.md
§"Rebase-sensitive invariants"](../../AGENTS.md).
