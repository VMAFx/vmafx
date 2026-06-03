# ADR-0763 — CUDA `adm_decouple` kernels: `__ldg()` F3 fix

| Field | Value |
| ------ | ----- |
| Status | Accepted |
| Date | 2026-05-29 |
| Tags | cuda, performance, adm, fork-local |

## Context

The struct-by-value kernel audit (Research-0756 / ADR-0763 batch) identified
`adm_decouple_kernel` and `adm_decouple_s123_kernel` in
`core/src/feature/cuda/integer_adm/adm_decouple.cu` as carrying the F3
pattern: both kernels accept `AdmBufferCuda buf` by value. The sub-struct
band pointers (`ref_dwt2.band_h/v/d`, `dis_dwt2.band_h/v/d`, `decouple_r`,
`decouple_a`) are accessed once each per thread but are not extracted as
`__restrict__` raw pointers before the inner body. Without `__restrict__`
extraction, the CUDA compiler cannot classify the underlying device-memory
loads as alias-free and therefore cannot route them through the L1 read-only
texture cache via `__ldg()`. This mirrors the finding documented in ADR-0743
(VIF filter1d) and ADR-0754 (SSIM vert_combine).

Note: `adm_decouple.cu` is not currently compiled into the active build — the
decouple computation was inlined into `adm_csf.cu` and `adm_cm.cu` via
`adm_decouple_inline.cuh` to eliminate intermediate buffers. The F3 fix is
applied as a preparatory maintenance change so the file remains consistent
with the rest of the codebase and is ready if re-integrated.

## Decision

Apply the F3 `__ldg()` fix to both kernels in `adm_decouple.cu`:

1. Extract `const int16_t *__restrict__` (scale-0) or
   `const int32_t *__restrict__` (scales 1-3) pointers from each read-only
   sub-struct band pointer before the `if (i < bottom && j < right)` body.
2. Introduce a single `const int idx = i * stride + j` index variable to
   avoid repeating the multiply.
3. Replace every per-pixel read (`ref->band_h[i * stride + j]`, etc.) with
   `__ldg(&ptr[idx])`.
4. For the write-back side (`decouple_r` and `decouple_a` bands), extract
   plain non-`const` raw pointers and write directly (`r_h[idx] = rst_h`).
   `__ldg()` is not applied to writes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| No-op (skip since file is dead) | Zero risk | File drifts from codebase standard; re-integration inherits stale patterns | File is maintained in-tree; consistency matters |
| Pass `const AdmBufferCuda *buf` by pointer instead | Eliminates struct-copy overhead | Larger signature change; requires caller-side update | F3 pointer-extraction is lower-risk and mirrors ADR-0743 / ADR-0754 precedent |
| Full pointer-parameter refactor (F1+F3 combined) | Best for active kernels | `adm_decouple.cu` is dead code; the active compute is in `adm_csf.cu` | Out of scope for this targeted maintenance PR |

## Consequences

- Both kernels now extract all six read-only band pointers as
  `const T *__restrict__` before the per-pixel body.
- All per-pixel reads use `__ldg(&ptr[idx])`.
- Write-back uses plain raw pointers; no `__ldg()` on stores.
- The change is zero-risk for scores: the file is not compiled into the
  active build.
- Invariant: the pattern is consistent with ADR-0743, ADR-0754, and the
  `__ldg()` section of `core/src/feature/cuda/AGENTS.md`.

## References

- req: user direction 2026-05-29: "Apply the F3 fix ONLY to
  adm_decouple_kernel + adm_decouple_s123_kernel in adm_decouple.cu. Mirror
  PR #93's pattern."
- ADR-0743: VIF filter1d `__ldg()` precedent.
- ADR-0754: SSIM `vert_combine` `__ldg()` precedent (PR #93).
- ADR-0214: GPU-parity CI gate (places=4).
- Research-0756: F3 struct-by-value kernel audit.
