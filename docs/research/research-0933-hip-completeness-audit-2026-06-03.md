<!--
  Research digest — read-only triage, no fix PR.
  ADR-0108 deliverable (1) for any future T7-10b follow-up PR.
-->

# HIP Backend Completeness Audit — 2026-06-03

**Scope:** `core/src/feature/hip/` — all host `.c` files and `.hip` kernel
sources. The audit answers three questions:

1. Which features have real HIP kernels (`.hip` source compiled by hipcc into
   a `.hsaco` blob) registered in `core/src/meson.build::hip_kernel_sources`?
2. Which features still return `-ENOSYS` stubs when `enable_hipcc=true`?
3. Which `.hip` kernel sources exist on disk but are orphaned (not wired into
   the meson build or not called from any host extractor)?

---

## 1. How the HIP dispatch works

Two independent gates control whether a HIP extractor runs:

**Gate 1 — kernel availability (`HAVE_HIPCC`).** Every host `.c` file wraps its
real dispatch in `#ifdef HAVE_HIPCC … #else return -ENOSYS; #endif`. When
`enable_hipcc=false` (the default on non-ROCm CI agents), `HAVE_HIPCC` is not
defined and every lifecycle function stubs out unconditionally. When
`enable_hipcc=true`, real hipcc-compiled HSACO blobs are embedded via `xxd` and
loaded at init via `hipModuleLoadData`.

**Gate 2 — auto-dispatch flag (`VMAF_FEATURE_EXTRACTOR_HIP`).** When a caller
invokes `vmaf_use_features_from_model()` on a VmafContext that has an imported
HIP state, `compute_fex_flags()` sets the `VMAF_FEATURE_EXTRACTOR_HIP` bit.
`vmaf_get_feature_extractor_by_feature_name()` then prefers extractors whose
`.flags` field includes that bit (first pass). If none are found, it falls back
to the CPU twin (ADR-0530 fallback — intentional while HIP coverage is partial).

Extractors with Gate 1 satisfied but Gate 2 **not** set can be called directly
by name (`vmaf_use_feature_with_name`), but model-driven dispatch silently
routes them to the CPU twin.

---

## 2. Group A — Fully functional (both gates satisfied)

| Feature extractor | Kernel(s) in meson | `.flags` |
| --- | --- | --- |
| `integer_motion_hip` | `motion_score.hip` | `TEMPORAL \| HIP` |
| `integer_vif_hip` | `vif_statistics.hip` | `HIP` |
| `speed_chroma_hip` | `speed_score.hip` | `HIP` |
| `speed_temporal_hip` | `speed_score.hip` | `TEMPORAL \| HIP` |
| `ssimulacra2_hip` | `ssimulacra2_blur.hip`, `ssimulacra2_mul.hip` | `HIP` |
| `float_vif_hip` | `float_vif_score.hip` | `HIP` if `FLOAT_VIF_HIP_AUTODISPATCH` else `0` |

`float_vif_hip` is a conditional entry: the Meson option
`enable_float_vif_hip_autodispatch` (default `false`) gates the
`VMAF_FEATURE_EXTRACTOR_HIP` flag via
`#if defined(FLOAT_VIF_HIP_AUTODISPATCH)`. With the default build the kernel
compiles and loads but model-driven dispatch still routes to the CPU twin.

---

## 3. Group B — Real kernels compiled but auto-dispatch gated off

These features satisfy Gate 1 (`enable_hipcc=true` → real dispatch path, no
`-ENOSYS` from `#ifndef HAVE_HIPCC`). However their `.flags = 0` means Gate 2
is not satisfied. Model-driven dispatch silently falls back to the CPU twin.
Direct by-name invocation works.

The blocking dependency is the picture-pool buffer-type plumbing task T7-10c
(referenced as T7-10b in most file comments). Until that lands, pictures arrive
as CPU `VmafPicture` objects and each extractor performs its own explicit HtoD
copy at submit time; the `VMAF_FEATURE_EXTRACTOR_HIP` bit is intentionally held
back to avoid confusing the buffer-type dispatch check in
`feature_extractor.c::vmaf_feature_extractor_context_submit()`.

| Feature extractor | Kernel(s) in meson | `-ENOSYS` w/ hipcc=true | Blocker |
| --- | --- | --- | --- |
| `ciede_hip` | `ciede_score.hip` | No | T7-10b |
| `float_adm_hip` | `float_adm_score.hip` | No | T7-10b |
| `float_moment_hip` | `moment_score.hip` (float) | No | T7-10b |
| `float_motion_hip` | `float_motion_score.hip` | No | T7-10b |
| `float_psnr_hip` | `float_psnr_score.hip` | No | T7-10b |
| `float_ssim_hip` | `ssim_score.hip` | No | T7-10b |
| `integer_adm_hip` | `adm_dwt2/csf/csf_den/cm.hip` | No | T7-10b |
| `integer_cambi_hip` | `cambi_score.hip` | No | (same posture) |
| `integer_motion_v2_hip` | `motion_v2_score.hip` | No | T7-10b |
| `integer_ms_ssim_hip` | `ms_ssim_score.hip` | No | T7-10b |
| `integer_psnr_hip` | `psnr_score.hip` | No | (same posture) |
| `integer_psnr_hvs_hip` | `psnr_hvs_score.hip` | No | (same posture) |
| `integer_ssim_hip` | `integer_ssim_score.hip` | No | (same posture) |

**Clarification on `-ENOSYS` location.** Every file in this group contains
`return -ENOSYS` statements, but every one of those statements is either:

- inside an `#else` branch (the `HAVE_HIPCC=false` stub path), or
- inside a HIP error-code mapping helper
  (`case hipErrorNotSupported: return -ENOSYS`)
  which is dead at runtime unless the hardware reports that specific error
  code.

No extractor in Group B returns `-ENOSYS` from `init_fex_hip`,
`submit_fex_hip`, or `collect_fex_hip` when `HAVE_HIPCC` is defined.

---

## 4. Group C — Orphaned `.hip` sources

### 4.1 `integer_adm/adm_decouple.hip` — dead kernel port

- Declares two `__global__` kernels: `adm_decouple_kernel` and
  `adm_decouple_s123_kernel`.
- **Not** listed in `hip_kernel_sources` in `core/src/meson.build`.
- **Not** referenced from `integer_adm_hip.c`. No `hipModuleLoadData` call
  targets an `adm_decouple_hsaco` symbol anywhere in the tree.
- `integer_adm_hip.c` uses only `adm_dwt2`, `adm_csf`, `adm_csf_den`, and
  `adm_cm` kernels. The decouple step appears to have been absorbed into other
  kernels or handled by the CPU path.
- Verdict: **dead code** — 228 lines that compile under hipcc but are never
  loaded at runtime. The ADR-0539 cleanup that removed the weak HSACO stubs did
  not remove this source file.

### 4.2 `integer_adm/adm_decouple_inline.hip` — correctly used shared header

- Included via `#include "adm_decouple_inline.hip"` by `adm_csf.hip` and
  `adm_cm.hip`.
- Not a standalone kernel target; acts as a shared inline helper.
- Status: correct, no gap.

### 4.3 `integer_moment_score` meson key — dead build output

- `core/src/meson.build` registers `'integer_moment_score'` pointing to
  `hip/integer_moment/moment_score.hip`. The meson comment says
  "consumed by `integer_moment_hip.c`".
- However, `integer_moment_hip.c` was **deleted** by ADR-0546
  (`chore/hip-cuda-orphan-tu-cleanup`, 2026-05-18) as a duplicate of
  `float_moment_hip.c`. Only `integer_moment_hip.h` remains (it declares the
  extern HSACO symbol but no host extractor `.c` file includes it).
- No C file in the tree references `integer_moment_score_hsaco`.
- When `enable_hipcc=true` the meson custom_target still compiles
  `integer_moment/moment_score.hip` into a `.hsaco` and embeds it, but no
  translation unit links against it. There is no link error because the symbol
  is never referenced.
- Verdict: **dead build output** — the meson entry should be removed, and
  `integer_moment_hip.h` should be removed or reassigned, in a future cleanup
  PR. The float_moment HSACO (`'moment_score'` key →
  `float_moment/moment_score.hip`) is the live path consumed by
  `float_moment_hip.c`.

---

## 5. Summary table (all 19 host extractors)

| File | Kernels in meson | HIP flag set | -ENOSYS (hipcc=true) | Group |
| --- | --- | --- | --- | --- |
| `ciede_hip.c` | `ciede_score` | No | No | B |
| `float_adm_hip.c` | `float_adm_score` | No | No | B |
| `float_moment_hip.c` | `moment_score` | No | No | B |
| `float_motion_hip.c` | `float_motion_score` | No (TEMPORAL only) | No | B |
| `float_psnr_hip.c` | `float_psnr_score` | No | No | B |
| `float_ssim_hip.c` | `ssim_score` | No | No | B |
| `float_vif_hip.c` | `float_vif_score` | Conditional (ADR-0623) | No | A (cond.) |
| `integer_adm_hip.c` | `adm_dwt2/csf/csf_den/cm` | No | No | B |
| `integer_cambi_hip.c` | `cambi_score` | No | No | B |
| `integer_motion_hip.c` | `motion_score` | Yes | No | A |
| `integer_motion_v2_hip.c` | `motion_v2_score` | No (TEMPORAL only) | No | B |
| `integer_ms_ssim_hip.c` | `ms_ssim_score` | No | No | B |
| `integer_psnr_hip.c` | `psnr_score` | No | No | B |
| `integer_psnr_hvs_hip.c` | `psnr_hvs_score` | No | No | B |
| `integer_ssim_hip.c` | `integer_ssim_score` | No | No | B |
| `integer_vif_hip.c` | `vif_statistics` | Yes | No | A |
| `speed_chroma_hip.c` | `speed_score` | Yes | No | A |
| `speed_temporal_hip.c` | `speed_score` | Yes | No | A |
| `ssimulacra2_hip.c` | `ssimulacra2_blur/mul` | Yes | No | A |

---

## 6. Gaps requiring future action

### GAP-HIP-1: 13 extractors missing `VMAF_FEATURE_EXTRACTOR_HIP` flag (T7-10b)

All Group B extractors have real hipcc-compiled kernels and real dispatch code,
but the `VMAF_FEATURE_EXTRACTOR_HIP` bit is held back pending the picture-pool
buffer-type plumbing (T7-10c). The model-driven dispatch silently routes these
features to CPU twins. This is intentional but represents missing throughput
on AMD GPUs.

Fix path: implement T7-10c (picture pool plumbing), then flip the flag for each
extractor in a separate PR with cross-backend parity verification.

**Three extractors** (`integer_psnr_hvs_hip`, `integer_cambi_hip`,
`integer_ssim_hip`) do not mention T7-10b in their comments but have the same
`flags=0` posture. These should be confirmed as also blocked on T7-10c or
flagged individually.

### GAP-HIP-2: `adm_decouple.hip` — dead kernel code (228 lines)

`core/src/feature/hip/integer_adm/adm_decouple.hip` defines two kernels that
are never referenced. Should be removed in a cleanup PR (no functional impact;
pure dead code reduction). ADR-0539 removed the weak stubs but missed this file.

### GAP-HIP-3: `integer_moment_score` meson entry — stale after ADR-0546

The `'integer_moment_score'` key in `hip_kernel_sources` and the file
`core/src/feature/hip/integer_moment_hip.h` are orphaned: their intended
consumer (`integer_moment_hip.c`) was deleted by ADR-0546. Should be removed
together in a cleanup PR. No link error occurs because the symbol is never
referenced, but the meson entry wastes one `hipcc --genco` invocation per build.

---

## 7. No `-ENOSYS` stubs active with `enable_hipcc=true`

To directly answer the task's third question: **no extractor returns `-ENOSYS`
from `init()`, `submit()`, or `collect()` when `enable_hipcc=true`**. Every
`-ENOSYS` path in `#ifdef HAVE_HIPCC`-guarded code is a HIP error-code
mapping helper (`hipErrorNotSupported → -ENOSYS`), not an unconditional stub.
All unconditional `-ENOSYS` returns are in `#ifndef HAVE_HIPCC` or `#else`
branches that are compiled out when `HAVE_HIPCC` is defined.

---

## 8. Files inspected

- `core/src/feature/hip/*.c`, `*.h` (all 19 host extractors + stubs file)
- `core/src/feature/hip/**/*.hip` (25 files)
- `core/src/meson.build` (lines 127–335, `hip_kernel_sources` dict)
- `core/src/feature/feature_extractor.c` (dispatch selection, lines 432–474)
- `core/src/libvmaf.c` (`compute_fex_flags`, lines 1465–1487)
- `core/src/feature/hip/AGENTS.md` (ADR-0546 deleted-orphan list)
