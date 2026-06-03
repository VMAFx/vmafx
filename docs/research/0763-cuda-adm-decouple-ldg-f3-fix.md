# Research-0763 — CUDA `adm_decouple` F3 `__ldg()` fix

**Date:** 2026-05-29
**Branch:** perf/cuda-adm-decouple-ldg-20260529
**Status:** Complete

## Summary

This is a targeted maintenance fix, not a measurement-driven optimisation.
The F3 finding (struct-by-value kernel obscures pointer alias-analysis) was
already documented in ADR-0743 and ADR-0754. This digest records the
file-status check that motivated applying the pattern here.

## File status

`core/src/feature/cuda/integer_adm/adm_decouple.cu` contains two kernels:

| Kernel | Band type | Status in build |
| --- | --- | --- |
| `adm_decouple_kernel` | `int16_t` (scale 0) | **Dead** — not in meson.build |
| `adm_decouple_s123_kernel` | `int32_t` (scales 1-3) | **Dead** — not in meson.build |

The active decouple computation lives in `adm_decouple_inline.cuh` and is
inlined into `adm_csf.cu` and `adm_cm.cu` (rebase-notes §0031). The standalone
`adm_decouple.cu` was retained as a reference/archive.

## Pattern applied

For each kernel:

- 6 read-only sub-struct band pointers extracted as `const T *__restrict__`
  before the `if (i < bottom && j < right)` block.
- 6 per-pixel reads converted to `__ldg(&ptr[idx])`.
- 6 write-back pointers extracted as plain `T *` (no `__ldg()` on stores).
- Single `const int idx = i * stride + j` avoids repeating the multiply.

## Correctness

Since the file is not compiled, there is no score impact. The correctness gate
(ADR-0214 places=4) was verified on the active `integer_adm` CUDA path
(unmodified) to confirm the build was not disturbed:

```text
# 576×324 Netflix golden pair (CUDA vs CPU):
integer_adm2 max diff: 1.00e-06  — places=4 PASS

# 1920×1080 checkerboard pair (CUDA vs CPU):
integer_adm2 max diff: 0.00e+00  — places=4 PASS
```

## Prior art

- ADR-0743: VIF `filter1d` — first `__ldg()` F3 application.
- ADR-0754: SSIM `vert_combine` — F2/F4, same extraction pattern (PR #93).
- rebase-notes §0031: `adm_decouple.cu` orphan status documented.
