# Research digest — HIP integer_moment HSACO blob registration (ADR-0539)

- **Date**: 2026-05-18
- **Author**: lusoris + claude
- **Status**: completed
- **Trigger**: user task — "port 3 HIP kernel sources from CUDA; no stubs anywhere"

## Question

Which of the three target extractors (`integer_psnr_hip`,
`integer_psnr_hvs_hip`, `integer_moment_hip`) still lacks a real HIP
HSACO blob, and what is the minimal change to satisfy all three host
TUs without introducing weak stubs?

## Findings

1. `core/src/feature/hip/integer_psnr/psnr_score.hip` — real kernel
   already exists (~133 LoC). Registered in meson under key `psnr_score`.
   Emits symbol `psnr_score_hsaco`. Consumed by
   `integer_psnr_hip.c::hipModuleLoadData(..., psnr_score_hsaco)`.
   **Status: complete pre-existing**.
2. `core/src/feature/hip/integer_psnr_hvs/psnr_hvs_score.hip` — real
   kernel already exists (~357 LoC). Registered under key
   `psnr_hvs_score`. Emits `psnr_hvs_score_hsaco`. Consumed by
   `integer_psnr_hvs_hip.c`. **Status: complete pre-existing**.
3. `core/src/feature/hip/integer_moment/moment_score.hip` — real
   kernel already exists (~149 LoC). **Not registered.** The meson key
   `moment_score` resolves to a different file (`hip/float_moment/
   moment_score.hip` — the float twin), so no `integer_moment_score_hsaco`
   symbol was emitted, and `integer_moment_hip.c` referenced an undefined
   symbol. No weak stub backed it either (`hip_hsaco_stubs.c` only
   covers the four ADM kernels per ADR-0536). **Status: needs meson
   registration.**

## Standalone compile probe

Verified all three sources compile to HSACO via direct hipcc invocation:

```bash
hipcc --genco --offload-arch=gfx1100 \
  -I libvmaf/src -I core/src/feature \
  -I core/src/feature/hip -I core/src/hip \
  core/src/feature/hip/integer_moment/moment_score.hip \
  -o /tmp/integer_moment_score.hsaco
# → 15200 bytes, no errors
```

Same for `psnr_score.hip` (12600 bytes) and `psnr_hvs_score.hip`
(31048 bytes).

## Minimal fix

Add one entry to `hip_kernel_sources` in `core/src/meson.build`:

```meson
'integer_moment_score' : feature_src_dir + 'hip/integer_moment/moment_score.hip',
```

with an inline comment distinguishing it from the existing
`moment_score` (float_moment) key.

## Verification

End-to-end (`vmaf --backend hip|cpu --feature psnr|psnr_hvs|float_moment`
on `src01_hrc00 ↔ src01_hrc01` 576×324, 48 frames):

- All emitted scores (`psnr_y/cb/cr`, `psnr_hvs / psnr_hvs_y/cb/cr`,
  `float_moment_ref1st/dis1st/ref2nd/dis2nd`) delta = 0.000000.
- Well within the places=4 places gate; in fact bit-exact.

## Conclusion

Single-line meson change + ADR + doc/changelog/index updates. No new
weak stub. Three host TUs all resolve via real HSACO blobs.
