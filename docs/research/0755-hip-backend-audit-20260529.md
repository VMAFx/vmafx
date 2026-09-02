<!-- markdownlint-disable MD013 MD060 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
# Research-0755 — HIP Backend Audit (2026-05-29)

## Purpose

Full audit of `core/src/hip/` (runtime, picture, dispatch) and
`core/src/feature/hip/` (feature extractors + kernels) for:

1. `extern "C"` name-mangling correctness (same class as CUDA SSIM P0, PR #80).
2. Pinned-host buffer leak pattern (same class as PR #94).
3. `AdmBufferHip` struct-by-value in kernel signatures (same class as PR #93 F3).
4. Scaffold vs real-kernel classification for every registered extractor.

## Scope

Files read:

- `core/src/hip/{common,picture_hip,dispatch_strategy,kernel_template}.c`
- `core/src/feature/hip/*.c` (20 host TUs)
- `core/src/feature/hip/**/*.hip` (25 kernel files)
- `core/src/hip/AGENTS.md`, `core/src/feature/hip/AGENTS.md`

---

## Inventory

### Runtime (core/src/hip/)

| File | Status |
|---|---|
| `common.c` | Real: `vmaf_hip_state_init`, `vmaf_hip_state_free`, `vmaf_hip_list_devices` all wired to HIP runtime. `vmaf_hip_import_state` lives in `libvmaf.c` (ADR-0519). |
| `picture_hip.c` | Real under `HAVE_HIPCC`: `hipMalloc` / `hipFree`. Returns `-ENOSYS` without `HAVE_HIPCC` (correct). |
| `dispatch_strategy.c` | Stub: `vmaf_hip_dispatch_supports` returns 0 unconditionally. Comment notes a feature-name→kernel table is pending. |
| `kernel_template.c` | Real: lifecycle/readback helpers fully wired. |

### Feature extractors — real vs scaffold

All 20 host TUs have `HAVE_HIPCC` guards. Under `enable_hipcc=true` they
load an HSACO blob via `hipModuleLoadData`, resolve kernels via
`hipModuleGetFunction`, and dispatch via `hipModuleLaunchKernel`. Under
`enable_hipcc=false` `init()` returns `-ENOSYS` (correct scaffold posture).

Extractors with confirmed real kernels (`.hip` file registered in
`hip_kernel_sources`, strong HSACO symbol overrides weak stub):

| Extractor | Kernel file | ADR |
|---|---|---|
| `integer_psnr_hip` | `integer_psnr/psnr_score.hip` | ADR-0372 |
| `float_psnr_hip` | `float_psnr/float_psnr_score.hip` | ADR-0254 |
| `float_ansnr_hip` | (ansnr kernel) | ADR-0372 |
| `float_motion_hip` | `float_motion/float_motion_score.hip` | ADR-0373 |
| `float_ssim_hip` | `float_ssim/ssim_score.hip` | ADR-0274 |
| `integer_ssim_hip` | `integer_ssim/integer_ssim_score.hip` | ADR-0564 |
| `integer_adm_hip` | `integer_adm/adm_{dwt2,csf,csf_den,cm,decouple}.hip` | ADR-0539 |
| `integer_cambi_hip` | `integer_cambi/cambi_score.hip` | ADR-0345/ADR-0360 |
| `ciede_hip` | `integer_ciede/ciede_score.hip` | ADR-0259 |
| `float_moment_hip` | `float_moment/moment_score.hip` | ADR-0260 |
| `integer_motion_hip` | `integer_motion/motion_score.hip` | ADR-0523 |
| `integer_motion_v2_hip` | `integer_motion_v2/motion_v2_score.hip` | ADR-0267 |
| `float_adm_hip` | `float_adm/float_adm_score.hip` | ADR-0468 |
| `float_vif_hip` | `float_vif/float_vif_score.hip` | — |
| `integer_vif_hip` | `integer_vif/vif_statistics.hip` | ADR-0537 |
| `integer_ms_ssim_hip` | `integer_ms_ssim/ms_ssim_score.hip` | — |
| `integer_psnr_hvs_hip` | `integer_psnr_hvs/psnr_hvs_score.hip` | — |
| `speed_chroma_hip` | `speed/speed_score.hip` | ADR-0567 |
| `speed_temporal_hip` | `speed/speed_score.hip` | ADR-0567 |
| `ssimulacra2_hip` | `ssimulacra2/ssimulacra2_{blur,mul}.hip` | — |

Note: per `core/src/hip/AGENTS.md` the VMAF_FEATURE_EXTRACTOR_HIP flag is
still cleared on most extractors because end-to-end CLI reproducers are
pending. `dispatch_strategy.c` returns 0 (nothing dispatches), so all
frames currently fall through to CPU twins regardless.

### extern "C" status — all kernel files

Audited all 25 `.hip` kernel files. The critical question is whether
`__global__` kernel functions that are looked up by name via
`hipModuleGetFunction` are wrapped in `extern "C"`.

**Four files use `#define`-then-instantiate pattern** where the
`__global__` token appears inside a macro definition body before the
enclosing `extern "C" { ... }` block instantiates the macro:

| File | Pattern |
|---|---|
| `integer_adm/adm_csf.hip` | `#define ADM_CSF_KERNEL(...) __global__ void ...` then `extern "C" { ADM_CSF_KERNEL(1,4); }` |
| `integer_adm/adm_csf_den.hip` | Same pattern with `ADM_CSF_SCALE_LINE` / `ADM_CSF_DEN_S123_LINE` |
| `integer_adm/adm_dwt2.hip` | Same pattern with `DWT_S123_COMBINED_VERT`, `DWT_8_VERT_HORI` macros |
| `integer_adm/adm_cm.hip` | Multiple extern-C-qualified `__global__` kernels plus inline templates — see detail below |

These are **NOT broken**: the `__global__` in the macro body is a textual
pattern; the actual function instantiation happens inside `extern "C" { }`.
The generated symbol names (e.g. `adm_csf_kernel_1_4`) are therefore
unmangled. No `extern "C"` bug exists in these files.

`speed/speed_score.hip` uses `extern "C" __global__ void ...` per-function
(inline linkage declaration) rather than a block. This is equally correct.

All other `.hip` files use the block form `extern "C" { __global__ void ...
}`. **No extern "C" name-mangling bug was found.**

---

## Findings

### P0 — No blockers found for extern "C"

The concern from the CUDA SSIM P0 (PR #80) does not reproduce in the HIP
backend. Every `__global__` kernel function is correctly wrapped via either
`extern "C" { }` block or per-function `extern "C" __global__ void`.

### P0 — Pinned-host leak: FIXED in kernel_template.c

`vmaf_hip_kernel_readback_free` (kernel_template.c:219–245) correctly frees
both `rb->device` via `hipFree` and `rb->host_pinned` via `hipHostFree`
before zeroing both pointers. The CUDA pattern that PR #94 fixed
(`vmaf_cuda_kernel_readback_free` leaked `host_pinned`) is not replicated
here. No leak found.

### P1 — AdmBufferHip struct passed by value in kernel signatures (F3 pattern)

**File:** `core/src/feature/hip/integer_adm/adm_csf.hip` line 59, 128, 186, 195
**File:** `core/src/feature/hip/integer_adm/adm_cm.hip` lines 186, 277, 382+

`AdmBufferHip` is defined in `integer_adm_hip.h:70–96` and contains:

- 2 × `size_t` (16 bytes on x86-64)
- 6 × `hip_adm_dwt_band_t` structs (each contains 4 pointers: 32 bytes; ×6 = 192 bytes)
- 8 × pointers (64 bytes)
- Total: ~272 bytes

Passing a 272-byte struct by value to a `__global__` kernel means each
thread receives a full copy on its stack via the kernel-argument buffer
path. On RDNA/GCN this results in significant argument-passing overhead.
More critically, the device code treats the embedded pointers as device
addresses; if the struct is large enough to push past the 4096-byte kernel
argument limit on some GFX targets, the launch silently truncates.

**Recommendation (P1):** Replace `AdmBufferHip buf` parameters with a
`const AdmBufferHip * __restrict__ buf` pointer in `adm_csf.hip` and
`adm_cm.hip`, and allocate one `AdmBufferHip` in device-accessible constant
memory or pass as a `__constant__` parameter. This matches the CUDA twin's
`CUdeviceptr`-per-field pattern which avoids struct-by-value entirely.
Same fix shape as PR #93 F3 recommendation.

### P1 — `hipMalloc` used for per-frame staging buffers

**Affected files:** `float_ssim_hip.c`, `integer_ssim_hip.c`, `float_psnr_hip.c`,
`float_vif_hip.c`, `integer_ms_ssim_hip.c`, `integer_psnr_hip.c`,
`integer_psnr_hvs_hip.c`, `integer_cambi_hip.c`

All staging buffers allocated at init time (not per-frame), so the reviewer
criterion from the HIP reviewer system prompt ("per-frame allocations must
use `hipMallocAsync`") does not technically apply — these are one-shot
`init()` allocations amortised over the extractor lifetime. Noted for
completeness; not blocking. `picture_hip.c:vmaf_hip_picture_alloc` uses
synchronous `hipMalloc` which the comment at line 12–19 justifies.

### P2 — dispatch_strategy.c is still a complete stub

`vmaf_hip_dispatch_supports` returns 0 for every feature name. This means
the HIP dispatch mechanism is non-functional and every request routes to
CPU. This is intentional per ADR-0212 (scaffold posture) but should be
tracked. The `VMAF_FEATURE_EXTRACTOR_HIP` flag is cleared on all but
`vmaf_fex_integer_motion_hip` per AGENTS.md, so in practice CPU fallback
is the correct runtime path for now.

### P2 — CAMBI HIP has landed (ADR-0345 Phase 3 terminus)

Per the HIP reviewer scope notes, CAMBI HIP (`integer_cambi_hip.c` +
`integer_cambi/cambi_score.hip`) has landed. The HIP rolling-port series
is therefore complete. ADR-0345 Phase 3 terminus condition is met.

### P2 — No cross-backend ULP gate evidence

The system prompt requires every HIP feature kernel to land alongside a
cross-backend ULP gate (`places=4`) versus the CUDA twin per ADR-0214.
Evidence of gate runs is not present in the source tree for the newer
extractors (`integer_ssim_hip`, `integer_adm_hip`, `integer_cambi_hip`,
`ssimulacra2_hip`, `speed_*_hip`). The `docs/research/0754-*` and similar
files reference CUDA-side benchmarks only. Recommend running:

```bash
meson test -C build --suite=hip-parity
```

for each newly-promoted extractor before merge. Gate command per ADR-0214:

```bash
vmaf --backend hip --feature <name> \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --output /tmp/hip_score.json
# Compare with CUDA twin score; assert places=4
```

---

## Summary

| Priority | Count | Finding |
|---|---|---|
| P0 | 0 | No `extern "C"` mangling bugs; no pinned-host leaks |
| P1 | 2 | `AdmBufferHip` by-value in 7+ kernel signatures; `hipMalloc` for staging (non-blocking, init-time) |
| P2 | 3 | `dispatch_strategy.c` stub; missing ULP gate runs for newer kernels; CAMBI HIP terminus reached |

The HIP backend is substantially further along than the ADR-0212 scaffold
posture indicated: 20 extractors have real `hipModuleLoadData` paths,
all `extern "C"` kernel names are correct, and the pinned-host leak
pattern from PR #94 is absent.

## Reproducer commands

```bash
# Classify extern "C" status per kernel file
find core/src/feature/hip/ -name "*.hip" | while read f; do
    g=$(grep -n "__global__" "$f" | head -1 | cut -d: -f1)
    e=$(grep -n 'extern "C"' "$f" | head -1 | cut -d: -f1)
    echo "$f: first __global__=$g first_extern_c=$e"
done

# Find AdmBufferHip by-value kernel signatures
grep -rn "AdmBufferHip [a-z]" core/src/feature/hip/integer_adm/

# Confirm dispatch_strategy stub
grep -n "return" core/src/hip/dispatch_strategy.c
```
