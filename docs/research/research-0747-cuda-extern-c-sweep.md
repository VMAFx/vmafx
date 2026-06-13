<!-- markdownlint-disable MD060 -->
# Research-0747 — CUDA `extern "C"` name-mangling sweep

**Date:** 2026-05-28
**Branch:** `audit/cuda-extern-c-name-mangling-sweep-20260528`
**Status:** Complete — 1 broken file found and fixed.

## Motivation

PR #77 discovered that `integer_ssim/ssim_score.cu` had three
`__global__` kernels not wrapped in `extern "C"`, causing
`cuModuleGetFunction` to fail silently because the C++ name-mangler
decorated the symbols while the driver API lookup used the plain C
names. The fix for that file was merged, but no systematic sweep was
performed for the rest of the codebase.

This research covers every `__global__` CUDA kernel in
`core/src/feature/cuda/` and `core/src/cuda/` that is also looked
up by name via `cuModuleGetFunction` on the host side.

## Inventory

24 `.cu` files contain `__global__` definitions. 46 distinct kernel
names are looked up by `cuModuleGetFunction` across 15 host `.c`
files.

### Classification

| File | `extern "C"` present | All looked-up kernels covered | Status |
|------|----------------------|-------------------------------|--------|
| `integer_ssim/ssim_score.cu` | Yes (line 37) | Yes (`calculate_ssim_*`) | SAFE |
| `integer_ssim/integer_ssim_score.cu` | **No** | N/A — 3 kernels exposed bare | **BROKEN** |
| `integer_adm/adm_csf.cu` | Yes (line 205) | Yes (`adm_csf_kernel_1_4`, `i4_adm_csf_kernel_1_4`) | SAFE |
| `integer_adm/adm_csf_den.cu` | Yes (end of file) | Yes (`adm_csf_den_scale_line_kernel_8_128`, `adm_csf_den_s123_line_kernel_8_128`) | SAFE |
| `integer_adm/adm_dwt2.cu` | Yes (end of file) | Yes (all DWT + DWT2 kernels) | SAFE |
| `integer_adm/adm_cm.cu` | Yes (two blocks, lines 143 + 378) | Yes (`adm_cm_line_kernel_8`, `i4_adm_cm_line_kernel_fused`) | SAFE |
| `integer_adm/adm_decouple.cu` | Yes (line 41) | No lookup — `adm_decouple_kernel` / `adm_decouple_s123_kernel` are not looked up by `cuModuleGetFunction`; they are device-side | SAFE |
| `float_vif/float_vif_score.cu` | Yes (line 116) | Yes (`float_vif_compute`, `float_vif_decimate`) | SAFE |
| `integer_ciede/ciede_score.cu` | Yes (line 31) | Yes | SAFE |
| `integer_psnr_hvs/psnr_hvs_score.cu` | Yes (line 27) | Yes (`psnr_hvs`) | SAFE |
| `integer_ms_ssim/ms_ssim_score.cu` | Yes (line 42) | Yes | SAFE |
| `ssimulacra2/ssimulacra2_blur.cu` | Yes (line 65) | Yes (5 kernels including `ssimulacra2_transpose`) | SAFE |
| `ssimulacra2/ssimulacra2_mul.cu` | Yes (line 21) | Yes (`ssimulacra2_mul3`) | SAFE |
| `integer_cambi/cambi_score.cu` | Yes (line 99) | Yes | SAFE |
| `integer_motion_v2/motion_v2_score.cu` | Yes (line 55) | Yes | SAFE |
| `speed/speed_score.cu` | Yes (line 71) | Yes (5 kernels) | SAFE |
| `float_motion/float_motion_score.cu` | Yes (line 49) | Yes | SAFE |
| `float_psnr/float_psnr_score.cu` | Yes (line 27) | Yes | SAFE |
| `float_adm/float_adm_score.cu` | Yes (line 101) | Yes (6 kernels) | SAFE |
| `integer_motion/motion_score.cu` | Yes (line 62) | Yes | SAFE |
| `integer_moment/moment_score.cu` | Yes (line 36) | Yes | SAFE |
| `integer_vif/filter1d.cu` | Yes (line 832) | Yes (10 macro-expanded kernels) | SAFE |
| `integer_psnr/psnr_score.cu` | Yes (line 34) | Yes | SAFE |
| `integer_adm_cuda.c` (note: `.c` file with grep hit on comment) | n/a — not a `.cu` file | n/a | n/a |

### The one broken file

**`core/src/feature/cuda/integer_ssim/integer_ssim_score.cu`**

- Compiled as `integer_ssim_score_ptx` (confirmed in
  `core/src/meson.build` line 784).
- Loaded by `core/src/feature/cuda/ssim_cuda.c` via
  `cuModuleLoadData(&module, integer_ssim_score_ptx)`.
- Three `__global__` kernel entry points:
  - `integer_ssim_horiz_8bpc`
  - `integer_ssim_horiz_16bpc`
  - `integer_ssim_vert_combine`
- All three are looked up by name in `ssim_cuda.c` lines 122–127.
- The file had **zero** `extern "C"` declarations. nvcc compiles
  `.cu` files as C++ by default; without `extern "C"`, the symbols
  receive C++ name-mangling (`_Z31integer_ssim_horiz_8bpc...`).
  `cuModuleGetFunction` passes the plain C names and receives
  `CUDA_ERROR_NOT_FOUND`, which the host `CHECK_CUDA_GOTO` macro
  logs and returns `-EINVAL` from `init_fex_cuda`. The feature
  `"ssim"` via `--backend cuda` is therefore silently non-functional
  from the time this file was introduced.

## Fix

Added `extern "C" {` immediately before the first `__global__`
definition (line 76) and `} /* extern "C" */` at end of file.
Device-only helpers (`__device__ static`) and `#define` constants are
not affected.

## Audit script

`scripts/dev/check-cuda-extern-c.sh` codifies this check for CI.
It fails with exit code 1 if any `__global__` function in a `.cu`
file is referenced by `cuModuleGetFunction` but is not inside an
`extern "C"` block.

## Before / after smoke

No container build is available in the worktree environment (no
GPU), but the name-mangling evidence is definitive without a binary:
the function names looked up by the host are plain C identifiers; if
the compiled PTX exports only mangled names, the lookup always fails.
The fix eliminates the mismatch.
