<!-- markdownlint-disable MD013 MD060 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
# Research-0756: CUDA F3 Struct-by-Value Kernel Audit

- **Status**: Active
- **Workstream**: perf/cuda-f3-struct-by-value-audit (no ADR — research only)
- **Date**: 2026-05-29
- **References**: PR #93 (F3 finding deferral), Research-0754, Research-0734 through 0738

## Background

PR #93 (`perf/cuda-ssim-vert-combine-ldg-launch-bounds-leak-20260529`) identified
"F3" as the pattern where a `__global__` kernel accepts a `VmafCudaBuffer` (or
similar aggregating struct) **by value**. This hides the underlying `CUdeviceptr`
pointer from the compiler's alias analysis and prevents ptxas from classifying
those loads as non-aliased reads eligible for:

- the read-only L1 texture cache path (`ld.global.nc`), and
- `__restrict__`-based alias-free optimisation within the inner loop.

The fix used in PR #93 for `calculate_ssim_vert_combine` was to extract
`const float *__restrict__` raw pointers from each `VmafCudaBuffer` argument
before the inner loop and then read via `__ldg()`. That approach is measured
at -4.2% kernel duration at 1080p (Research-0754).

This digest performs the fork-wide inventory and severity-ranks the affected
kernels by their measured DRAM throughput from PR #77's ncu profiles.

---

## Struct definitions

| Struct | Fields | What hides the pointer |
|---|---|---|
| `VmafCudaBuffer` (`core/src/cuda/common.h:32`) | `size_t size; CUdeviceptr data;` | `data` field is `CUdeviceptr` (an opaque `unsigned long long`); the compiler cannot deduce non-alias from a struct copy |
| `VmafPicture` (`core/include/libvmaf/picture.h:40`) | `pix_fmt, bpc, w[3], h[3], stride[3], void *data[3], VmafRef *ref, void *priv` | `void *data[3]` — opaque void array; no restrict-path possible |
| `AdmBufferCuda` (`core/src/feature/cuda/integer_adm/integer_adm_cuda.h`) | Large struct of `cuda_adm_dwt_band_t` sub-structs containing `int16_t *` / `int32_t *` pointers | The containing struct copy blocks restrict analysis on all pointer members |

---

## Full kernel inventory

### AFFECTED — takes VmafCudaBuffer or similar struct by value

| File | Kernel | Struct args | Inner loop reads |
|---|---|---|---|
| `integer_ms_ssim/ms_ssim_score.cu:70` | `ms_ssim_decimate` | `VmafCudaBuffer src, VmafCudaBuffer dst` | 81 loads/output pixel via `reinterpret_cast<const float *>(src.data)` |
| `integer_ms_ssim/ms_ssim_score.cu:100` | `ms_ssim_horiz` | 7× `VmafCudaBuffer` | 11 loads per channel per output pixel (K=11), 7 buffers |
| `integer_ms_ssim/ms_ssim_score.cu:136` | `ms_ssim_vert_lcs` | 8× `VmafCudaBuffer` | 11 loads × 5 input buffers (K=11 vert Gaussian) |
| `integer_psnr_hvs/psnr_hvs_score.cu:198` | `psnr_hvs` | `VmafCudaBuffer ref_in, VmafCudaBuffer dist_in, VmafCudaBuffer partials_out` | 64-pixel DCT block reads from `ref_in.data`, `dist_in.data` |
| `integer_psnr/psnr_score.cu:36` | `calculate_psnr_kernel_8bpc` | `VmafCudaBuffer sse` (output only) | Write-only via `sse.data` — input is `VmafPicture` |
| `integer_psnr/psnr_score.cu:74` | `calculate_psnr_kernel_16bpc` | `VmafCudaBuffer sse` (output only) | Write-only — same as above |
| `integer_psnr/psnr_score.cu:36` | `calculate_psnr_kernel_8bpc` | `const VmafPicture ref, const VmafPicture dis` | `void *data[3]` pointer fields — no `__restrict__` possible |
| `integer_psnr/psnr_score.cu:74` | `calculate_psnr_kernel_16bpc` | `const VmafPicture ref, const VmafPicture dis` | Same |
| `integer_moment/moment_score.cu:54` | `calculate_moment_kernel_8bpc` | `const VmafPicture ref, const VmafPicture dis` | Per-pixel ref[x], dis[x] loads from `void *data[0]` |
| `integer_moment/moment_score.cu:87` | `calculate_moment_kernel_16bpc` | `const VmafPicture ref, const VmafPicture dis` | Same |
| `integer_ciede/ciede_score.cu:175` | `calculate_ciede_kernel_8bpc` | `const VmafPicture ref, const VmafPicture dis, VmafCudaBuffer sum` | 6 channel loads per pixel (Y+U+V × 2) |
| `integer_ciede/ciede_score.cu:218` | `calculate_ciede_kernel_16bpc` | Same | Same |
| `integer_ssim/ssim_score.cu:75` | `calculate_ssim_horiz_8bpc` | `const VmafPicture ref, const VmafPicture cmp` | pixel reads via `void *data[0]` |
| `integer_ssim/ssim_score.cu:112` | `calculate_ssim_horiz_16bpc` | Same | Same |
| `integer_adm/adm_decouple.cu:43` | `adm_decouple_kernel` | `AdmBufferCuda buf` | `buf.ref_dwt2.band_h[i*stride+j]` etc — 6 loads per pixel |
| `integer_adm/adm_decouple.cu:126` | `adm_decouple_s123_kernel` | `AdmBufferCuda buf` | Same pattern |
| `integer_adm/adm_cm.cu:144` | `i4_adm_cm_line_kernel_fused` | `AdmBufferCuda buf, AdmFixedParametersCuda params` | Multiple indirect reads via `buf.i4_ref_dwt2`, `buf.i4_csf_f` |
| `integer_adm/adm_cm.cu:365` | `adm_cm_line_kernel_N` (macro) | `AdmBufferCuda buf` | Per-pixel cubic accumulation loop |
| `integer_adm/adm_cm.cu:456` | `i4_adm_cm_aim_line_kernel_fused` | `AdmBufferCuda buf` | Same |
| `integer_adm/adm_cm.cu:682` | `adm_cm_aim_line_kernel_N` (macro) | `AdmBufferCuda buf` | Same |

**Partial F3 (VmafCudaBuffer is output-only or single read — lower severity):**

| File | Kernel | Struct args | Note |
|---|---|---|---|
| `integer_motion/motion_score.cu:64` | `calculate_motion_score_kernel_8bpc` | `VmafPicture src` + `VmafCudaBuffer src_blurred, prev_blurred, sad` | `src.data[0]` read once for tile load; `src_blurred` / `prev_blurred` written/read as `uint16_t *` in inner loop — F3 applies but kernel is already smem-tiled |
| `integer_motion_v2/motion_v2_score.cu:57` | `motion_v2_kernel_8bpc` | `VmafCudaBuffer sad` (output-only) | Only the atomic output is via struct — input pointers already `__restrict__` |

### SAFE-PTR — takes raw `const T * __restrict__` pointers

| File | Kernel | Note |
|---|---|---|
| `float_psnr/float_psnr_score.cu:29` | `float_psnr_kernel_8bpc` | `const uint8_t *__restrict__ ref/dis` — clean |
| `float_psnr/float_psnr_score.cu:63` | `float_psnr_kernel_16bpc` | Clean |
| `float_vif/float_vif_score.cu:123` | `float_vif_compute` | Raw `const uint8_t *ref_raw, *dis_raw` |
| `float_vif/float_vif_score.cu:283` | `float_vif_decimate` | Raw pointers |
| `float_adm/float_adm_score.cu:115–547` | All 6 float_adm kernels | Raw `const float *ref_band, *dis_band` |
| `integer_vif/filter1d.cu` | All 8 filter1d variants (macro-generated) | Struct-internal ptrs promoted to locals before inner loop (PR #74 pattern) |
| `integer_adm/adm_csf.cu` | `adm_csf_kernel_*`, `i4_adm_csf_kernel_*` | Raw int pointer args from template params |
| `integer_adm/adm_csf_den.cu` | Both scale-line kernels | Raw `int16_t *`, `int32_t *` in template |
| `integer_adm/adm_dwt2.cu` | All DWT2 kernels | Smem-tiled; raw ptrs from struct extracted to locals at kernel entry |
| `integer_cambi/cambi_score.cu` | All 3 cambi kernels | `const uint16_t *`, `uint16_t *` — raw ptrs |
| `ssimulacra2/ssimulacra2_blur.cu` | All 5 blur kernels | `const float *__restrict__` throughout |
| `ssimulacra2/ssimulacra2_mul.cu` | `ssimulacra2_mul3` | `__restrict__` on all 3 ptrs |
| `speed/speed_score.cu` | All 5 speed kernels | `const float *__restrict__` throughout |
| `integer_psnr_hvs/psnr_hvs_score.cu:198` | `psnr_hvs` inner reads | Shared-memory DCT ops after tile load — L1 resident |

---

## Severity ranking (DRAM throughput from PR #77 ncu measurements)

The critical question for F3 severity is: does the kernel do significant
global-memory reads in a hot inner loop, and are those reads currently missing
the read-only L1 cache path?

| Rank | Kernel | DRAM Tp (baseline ncu) | Source digest | F3 severity | Reason |
|---|---|---|---|---|---|
| 1 | `ms_ssim_vert_lcs` | Not independently measured; structural twin of `calculate_ssim_vert_combine` (55.8% at 1080p — Research-0736) | Research-0737 (inferred) | **HIGH** | 11-tap vert loop over 5 `VmafCudaBuffer` args (55 loads/pixel). No pointer extraction before loop. Identical pattern to `calculate_ssim_vert_combine` pre-PR-#93 |
| 2 | `ms_ssim_horiz` | Not independently measured; same kernel shape as `calculate_ssim_horiz_8bpc` (8.8% at 576p but scales with resolution) | Research-0737 (inferred) | **HIGH** | 7 `VmafCudaBuffer` args; 11-tap horizontal loop; F3 prevents `ld.global.nc` on all 7 intermediate buffers |
| 3 | `calculate_ciede_kernel_8bpc/16bpc` | Not measured in PR #77 digests (adm/motion/ssim/ms_ssim covered; ciede absent) | Research-0738 gap | **MEDIUM-HIGH** | 6 channel reads per pixel from `VmafPicture.data[0..2]` + `__restrict__`-invisible; also `ciede2000_dev()` is compute-heavy, reducing relative weight of F3 |
| 4 | `adm_decouple_kernel` / `adm_decouple_s123_kernel` | 7.8–7.9% DRAM (Research-0734; all ADM kernels launch-starved at 576p) | Research-0734 | **MEDIUM** | 6 pointer accesses per pixel from `AdmBufferCuda` sub-structs. At 1080p and above the DRAM pattern matters; launch starvation dominates at 576p |
| 5 | `psnr_hvs` | Not measured in PR #77 digests | Research-0738 gap | **MEDIUM** | 64-pixel DCT block loaded per CUDA block from `ref_in.data`, `dist_in.data`; post-load work is integer DCT in shared memory — F3 only affects the initial tile load |

**Below top-5:**

- `calculate_psnr_kernel_*`: `VmafCudaBuffer sse` is write-only; the
  `VmafPicture` inputs read at most 1 pixel/thread; launch-count and integer
  arithmetic dominate. F3 severity: LOW.
- `calculate_moment_kernel_*`: Same pattern as psnr — 2 loads per pixel,
  arith-light kernel, warp-reduce dominated. F3 severity: LOW.
- `adm_cm_line_kernel_8` / `i4_adm_cm_line_kernel_fused`: Already receiving
  `__launch_bounds__(128, 8)` attention (PR #97). The `AdmBufferCuda` struct
  access pattern is indirect (`buf.ref_dwt2.band_h[i*stride+j]`); the inner
  loop is already register-limited, not DRAM-limited. Adding `__ldg()` here
  requires extracting ~6 per-band sub-pointers which is more invasive without
  proportionate DRAM gain.
- `calculate_motion_score_kernel_8bpc`: Input is `VmafPicture.data[0]` but
  is loaded once per tile into shared memory. Post-smem-load inner loop
  does not re-read global memory. F3 severity: NEGLIGIBLE.
- `motion_v2_kernel_8bpc`: `VmafCudaBuffer sad` is write-only (single
  atomic). Input args already `const uint8_t *__restrict__`. F3 severity:
  NONE (only output struct).

---

## Fix strategies

### In-kernel `__ldg()` extraction (low-risk, proven in PR #93)

```c
// Before inner loop, once per kernel invocation:
const float *h_ref_mu = __ldg_ptr(reinterpret_cast<const float *>(h_ref_mu_buf.data));
// ... then use h_ref_mu[src_idx] throughout the loop
```

This is the approach PR #93 applied to `calculate_ssim_vert_combine`. It
requires no call-site changes, is bit-identical (measured: 0.00e+00 max diff
on all frames in Research-0754), and yields measurable improvement where the
kernel is DRAM-bound at usable wave counts (1080p: -4.2% duration).

### Host-side extraction (higher impact, more invasive)

Extract raw `CUdeviceptr` values from each `VmafCudaBuffer` on the host
**before** `cuLaunchKernel`, then pass as raw kernel arguments. This removes
the struct entirely from the kernel signature and allows ptxas to see
`const float * __restrict__` directly, enabling full `ld.global.nc`
without `__ldg()`. Requires changing both the `.cu` kernel signature and
every `cuLaunchKernel` call site in the corresponding `_cuda.c` dispatcher.
Higher effort, not bit-risk, but more invasive. Recommended only where the
kernel is also a candidate for AoS→SoA restructuring (F1, deferred in PR #93).

---

## Top-5 PRs to dispatch from this audit

| Priority | PR to dispatch | Target kernel(s) | Strategy | Estimated effort | Expected ncu gain (1080p) |
|---|---|---|---|---|---|
| 1 | `perf/cuda-ms-ssim-vert-lcs-ldg` | `ms_ssim_vert_lcs` | In-kernel `__ldg()` extraction of 5 read-only buffers before K=11 vert loop | ~30 LOC, 1 `.cu` file, 0 call-site changes | -4 to -6% kernel duration (identical structural pattern to PR #93 ssim_vert) |
| 2 | `perf/cuda-ms-ssim-horiz-ldg` | `ms_ssim_horiz` | In-kernel `__ldg()` for 7 `VmafCudaBuffer` args before K=11 horiz loop | ~35 LOC, same file | -3 to -5% at ≥1080p; negligible at 576p (wave-limited) |
| 3 | `perf/cuda-ciede-restrict-ldg` | `calculate_ciede_kernel_8bpc` + `_16bpc` | `const uint8_t *` extraction from `VmafPicture.data[0..2]` before per-pixel compute; `__ldg()` on Y/U/V reads | ~25 LOC per variant; needs ncu A/B to confirm (no existing baseline) | Unknown — profile first; `ciede2000_dev()` is compute-heavy so DRAM saving may be <3% |
| 4 | `perf/cuda-adm-decouple-ldg` | `adm_decouple_kernel` + `adm_decouple_s123_kernel` | Extract 6 sub-struct band pointers from `AdmBufferCuda` before inner pixel loop | ~20 LOC; ADM kernels are launch-starved at 576p so must profile at 1080p+ | Only meaningful at ≥1080p; at 576p launch starvation dominates (DRAM 7.8%, Research-0734) |
| 5 | `perf/cuda-psnr-hvs-ldg` | `psnr_hvs` | Extract `ref_in.data` + `dist_in.data` pointers before shared-memory tile load | ~10 LOC; tile load is the only global-read phase | Moderate; tile load is a one-time per-block cost; post-load is smem-resident |

---

## Kernels confirmed already fixed

- `calculate_ssim_vert_combine` (`ssim_score.cu:153`) — fixed in PR #93 /
  Research-0754. Measured -4.2% kernel duration at 1080p.

---

## ncu profile commands for unvalidated kernels

```bash
# ms_ssim_vert_lcs (needs separate launch-count to capture past decimate)
ncu -k "regex:ms_ssim_vert_lcs" --set basic --launch-count 10 \
  core/build-ncu/tools/vmaf --feature float_ms_ssim_cuda --backend cuda \
  --reference python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv \
  --distorted python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv \
  --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 -o /dev/null
# ncu section: LaunchStats + MemoryWorkloadAnalysis

# ciede (1080p, both bpc variants)
ncu -k "regex:calculate_ciede" --set basic --launch-count 6 \
  core/build-ncu/tools/vmaf --feature ciede_cuda --backend cuda \
  --reference python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv \
  --distorted python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv \
  --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 -o /dev/null
```

---

## Summary

**Total AFFECTED kernels (F3 pattern present)**: 20 kernel variants across 8 families.

**Of those, high severity (inner hot loop, not smem-tiled, no existing pointer extraction)**: 5 (ms_ssim_vert_lcs, ms_ssim_horiz, ciede 8/16bpc, adm_decouple).

**Already fixed**: `calculate_ssim_vert_combine` (PR #93).

**Dispatch priority**: PR-1 (`ms_ssim_vert_lcs`) first — identical pattern to PR #93,
highest confidence of a measurable win at 1080p.

## Related

- PR #93 / Research-0754 (F3 first fix — ssim_vert_combine)
- Research-0736 (SSIM ncu hotpath)
- Research-0737 (MS-SSIM ncu hotpath)
- Research-0734 (ADM ncu hotpath)
- Research-0738 (cross-metric summary)
- ADR-0756 (this audit)
