<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
# Research-0773: CUDA ADM decouple-inline — `__ldg()` F3 fix (active path)

**Date**: 2026-05-29
**Related ADR**: ADR-0773
**Author**: lusoris

## Summary

PR #106 (ADR-0763) applied the F3 `__ldg()` pattern to the dead-code
`adm_decouple.cu`. This research digest documents the same analysis applied
to the live path: the inline helpers in `adm_cm.cu` and the kernel templates
in `adm_csf.cu` that together handle every ADM CSF and CM frame dispatch.

## Background: why `adm_decouple.cu` is dead code

Rebase-note 0002 records that the fork eliminated the standalone decouple
kernel pass and its 6 intermediate GPU buffers (~107 MB at 4K) in an early
refactor. `adm_csf.cu` and `adm_cm.cu` now each `#include
"adm_decouple_inline.cuh"` and call the `__device__ __forceinline__` helpers
directly from inside their own per-pixel loops. The `.cu` file is still
compiled (it is in the Meson source list) but its two `__global__` functions
are never registered or dispatched.

## Load sites on the active path

Eight sites in two files perform raw `ref->band_h[idx]` / `dis->band_h[idx]`
reads (6 loads per call × 2 paths per scale × 4 scales per frame):

| File | Function | Type | Loads/call |
| --- | --- | --- | --- |
| `adm_csf.cu` | `i4_adm_csf_kernel<>` (inner loop) | int32 | 6 |
| `adm_csf.cu` | `adm_csf_kernel<>` (inner loop) | int16 | 6 |
| `adm_cm.cu` | `inline_i4_csf_a` | int32 | 6 |
| `adm_cm.cu` | `inline_i4_decouple_r` | int32 | 6 |
| `adm_cm.cu` | `inline_s0_csf_a` | int16 | 6 |
| `adm_cm.cu` | `inline_s0_decouple_r` | int16 | 6 |
| `adm_cm.cu` | `inline_i4_csf_r` (AIM path) | int32 | 6 |
| `adm_cm.cu` | `inline_s0_csf_r` (AIM path) | int16 | 6 |

All eight sites receive `const cuda_*_adm_dwt_band_t *` struct pointers
derived from an `AdmBufferCuda` that was passed by value to the enclosing
`__global__` kernel. The by-value copy hides the pointer from ptxas alias
analysis so `ld.global.nc` (read-only L1 texture cache path) is never emitted.

## Fix applied

For each of the eight functions: extract named `const T *__restrict__` pointers
from `ref->band_h`, `ref->band_v`, `ref->band_d`, `dis->band_h`, `dis->band_v`,
`dis->band_d` before the indexed load, then read via `__ldg(&ptr[idx])`.

Pattern (int32 example):

```cuda
const int32_t *__restrict__ rh = ref->band_h;
/* ... (rv, rd, dh, dv, dd) ... */
int32_t oh = __ldg(&rh[idx]);
```

## Correctness

This change is semantics-preserving. `__ldg()` on Compute Capability ≥ 3.5
emits `ld.global.nc`; on older hardware it falls back to `ld.global`. No
rounding, ordering, or precision change.

Expected parity vs CPU:

- 576×324 Netflix golden pair (`src01_hrc00/hrc01`): `integer_adm2` max diff
  ≤ 1.00e-06 (places=4 PASS)
- 1920×1080 checkerboard (1-px shift): `integer_adm2` max diff = 0.00e+00 (PASS)
- 1920×1080 checkerboard (10-px shift): `integer_adm2` max diff = 0.00e+00 (PASS)

## Performance model

Each ADM CSF dispatch reads 6 int32 (or int16) values per pixel per band.
The DWT2 planes for a 1080p frame are ~8 MB total (3 × H × V × 4 bytes for
int32). This exceeds L1 cache (typically 32–64 KB/SM) so cache-line reuse
within a thread block is limited. Routing to the read-only L1 texture cache
reduces L2 bandwidth demand and is consistent with the measured −4.2% kernel
duration reduction on SSIM vert_combine (Research-0754) for a similar
read-only load pattern.

No ncu A/B was run for this change; the benefit is expected to scale with
resolution and be minimal at 576p (dispatch-bound regime per Research-0760).
