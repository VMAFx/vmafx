<!-- markdownlint-disable MD060 -->
# Research: Dispatch-Strategy Registry Audit — 2026-05-15

## Summary

Full cross-reference audit of every backend's dispatch-strategy registry
against the `vmaf_fex_*` symbols actually built into libvmaf.  Three
categories of defect were found and fixed in this PR:

1. **Duplicate pointer entries** in `feature_extractor_list[]` for the
   SYCL and Vulkan backends (no coverage gap, but non-trivial bloat).
2. **Wrong feature-name strings** in `vmaf_metal_dispatch_supports()`'s
   `g_metal_features[]` table — the table was populated with placeholder
   names that do not match what the Metal extractors actually emit.
3. **Unconditionally-zero `vmaf_hip_dispatch_supports()`** — the function
   always returned 0 even though 8 HIP kernels are registered.

---

## Audit Scope

Every `VmafFeatureExtractor vmaf_fex_*` definition in `core/src/`
was extracted via `git grep`.  The resulting symbol sets were
cross-referenced against:

- `feature_extractor_list[]` in `core/src/feature/feature_extractor.c`
  (the central dispatch table that `vmaf_get_feature_extractor_by_name`
  and `vmaf_get_feature_extractor_by_feature_name` walk).
- `vmaf_<backend>_dispatch_supports()` in
  `core/src/<backend>/dispatch_strategy.c`
  (the per-backend predicate callers use to test whether a named feature
  can route to that backend before binding GPU pictures).

---

## Symbol-vs-Registry Cross-Reference Table

### CPU (baseline)

18 CPU extractors — all present in `feature_extractor_list[]`.
**Result: CLEAN.**

### CUDA

17 symbols defined in `core/src/feature/cuda/`.
All 17 appear exactly once in `feature_extractor_list[]`.
**Result: CLEAN.**

### SYCL

17 symbols defined in `core/src/feature/sycl/`.
All 17 appear in `feature_extractor_list[]`, but 6 were duplicated
(appeared twice): `psnr_sycl`, `psnr_hvs_sycl`, `float_ssim_sycl`,
`float_ms_ssim_sycl`, `float_moment_sycl`, `ciede_sycl`.

**Gap class: DUPLICATE — no missing coverage, but bloat. Fixed.**

### Vulkan

17 symbols defined in `core/src/feature/vulkan/`.
All 17 appear in `feature_extractor_list[]`, but the table contained
~62 pointer entries for 17 distinct symbols.  Worst cases:
`psnr_hvs_vulkan` and `float_ms_ssim_vulkan` appeared 11 times each.

**Gap class: DUPLICATE — no missing coverage, but severe bloat. Fixed.**

### HIP

8 symbols defined in `core/src/feature/hip/`:

| Symbol | `.name` field | In `feature_extractor_list[]` | In `vmaf_hip_dispatch_supports()` |
|--------|--------------|-------------------------------|-----------------------------------|
| `vmaf_fex_psnr_hip` | `"psnr_hip"` | yes | NO — returned 0 always |
| `vmaf_fex_float_psnr_hip` | `"float_psnr_hip"` | yes | NO |
| `vmaf_fex_ciede_hip` | `"ciede_hip"` | yes | NO |
| `vmaf_fex_float_moment_hip` | `"float_moment_hip"` | yes | NO |
| `vmaf_fex_float_ansnr_hip` | `"float_ansnr_hip"` | yes | NO |
| `vmaf_fex_integer_motion_v2_hip` | `"motion_v2_hip"` | yes | NO |
| `vmaf_fex_float_motion_hip` | `"float_motion_hip"` | yes | NO |
| `vmaf_fex_float_ssim_hip` | `"float_ssim_hip"` | yes | NO |

**Gap class: DISPATCH-SUPPORTS STUB never updated. Fixed.**

### Metal

8 symbols defined in `core/src/feature/metal/`.
All 8 in `feature_extractor_list[]`, but `g_metal_features[]` in
`vmaf_metal_dispatch_supports()` had wrong feature names:

| Old entry (wrong) | Correct entry | Source |
|-------------------|--------------|--------|
| `"motion2_v2_score"` | `"VMAF_integer_feature_motion2_v2_score"` | `integer_motion_v2_metal.mm` |
| *(missing)* | `"VMAF_integer_feature_motion_v2_sad_score"` | `integer_motion_v2_metal.mm` |
| `"float_motion"` | `"VMAF_feature_motion_score"` | `float_motion_metal.mm` |
| *(missing)* | `"VMAF_feature_motion2_score"` | `float_motion_metal.mm` |
| `"motion2_score"` | `"VMAF_integer_feature_motion_y_score"` | `integer_motion_metal.mm` |
| `"motion3_score"` | `"VMAF_integer_feature_motion2_score"` | `integer_motion_metal.mm` |
| *(missing)* | `"float_anpsnr"` | `float_ansnr_metal.mm` |
| `"float_ms_ssim"` *(spurious)* | *(removed)* | not provided by `float_ssim_metal` |

**Gap class: WRONG FEATURE NAMES in dispatch-supports table. Fixed.**

---

## Root Cause

The Metal table was populated during the ADR-0421 scaffolding pass with
approximate names.  The actual `provided_features[]` arrays in the `.mm`
files use the canonical `VMAF_*` prefixed names; the dispatch table was
never updated to match.

The HIP `vmaf_hip_dispatch_supports()` body retained its `TODO` stub
through all 8 consumer PRs because each consumer PR only added a kernel
and its `feature_extractor_list[]` registration — neither touched the
dispatch-supports predicate.

The SYCL/Vulkan duplicates appear to be copy-paste accidents during the
long sequence of backend-parity PRs; the lookup semantics (first-match)
masked them from any functional failure.

---

## Reproducer

Run `scripts/ci/check-dispatch-registry.sh` (added in this PR) to
compare the symbol set against the dispatch table for each backend and
report any gaps.

---

## Decision matrix

No novel routing decision is required: the missing entries are
mechanically determined by the extractor `.name` and `provided_features[]`
fields.  The SYCL/Vulkan deduplication is purely cosmetic — behaviour
is unchanged because the lookup returns on first match and every symbol
was already present at least once.
