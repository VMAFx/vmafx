# ADR-1025: R6 CUDA/HIP kernel correctness fixes

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `correctness`, `cuda`, `hip`, `simd`

## Context

Round-6 analysis found three correctness bugs in GPU kernel code:

1. **CUDA VIF filter1d.cu line 544 — copy-paste typo in rd-filter bound**:
   the upper-bound guard is `fwidth - (fwidth_rd - fwidth_rd) / 2` which
   always evaluates to `fwidth - 0 = fwidth`.  The correct expression is
   `fwidth - (fwidth - fwidth_rd) / 2`.  As a result, the 16-bit vertical
   VIF kernel uses all `fwidth` taps instead of the narrower `fwidth_rd`
   taps, indexing `vif_filt.filter[scale+1]` up to `fwidth_rd` entries
   beyond its valid range (OOB reads), producing wrong VIF scores at
   scales 0-2.  The 8-bit path at line 183 has the correct form.

2. **CUDA ADM adm_cm.cu line 344 — operator-precedence bug**:
   `(int64_t)accum_thread * accum_thread + add_shift_sq >> shift_sq` is
   parsed as `... + (add_shift_sq >> shift_sq)` = `... + 0` (since
   `add_shift_sq = 2^29, shift_sq = 30`), so the `>> shift_sq`
   normalisation is never applied to `accum_thread²`.  The reference
   macro at `integer_adm.c:743` has correct parenthesisation; this is
   a port defect.  Line 230 of the same file has the correct form.

3. **HIP adm_decouple.hip — missing function signature for
   `get_best15_from32`**: during the CUDA→HIP port the
   `__device__ __forceinline__ uint16_t get_best15_from32(uint32_t, int*)`
   declaration was dropped, leaving only a bare `{ ... return temp; }`
   block at file scope — invalid C++.  The function is called at lines
   167-169 of the same file.

4. **HIP vif_statistics.hip — wavefront reduce carries lost via OR**:
   `wavefront_reduce_i64` splits the 64-bit value into `uint32_t lo/hi`,
   reduces each half independently across 64 wavefront lanes, then
   reassembles with bitwise OR: `(int64_t)lo | ((int64_t)hi << 32)`.
   When the per-lane sum of `lo` exceeds `UINT32_MAX`, the carry into
   the upper word is silently discarded by the OR.  The fix uses integer
   addition: `(int64_t)((uint64_t)lo + ((uint64_t)hi << 32))`.

## Decision

Apply surgical one-liner / one-expression fixes for each:

- `filter1d.cu`: `fwidth_rd - fwidth_rd` → `fwidth - fwidth_rd`.
- `adm_cm.cu`: add inner parentheses so `>> shift_sq` normalises the
  full `accum²` expression.
- `adm_decouple.hip`: restore the `__device__ __forceinline__ uint16_t
  get_best15_from32(uint32_t temp, int *x)` signature before the body.
- `vif_statistics.hip`: change final OR to integer addition.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Rewrite `wavefront_reduce_i64` using a single `int64_t` shuffle | Simpler | `__shfl_xor` on `int64_t` requires HIP 5.7+ and is not guaranteed on all wavefront sizes | Stick with the split approach, fix the reassembly arithmetic |

## Consequences

- **Positive**: VIF, ADM scale-0, and AIM scores on the HIP path are
  numerically correct for all inputs.
- **Negative**: GPU parity snapshots under `testdata/` may require
  regeneration via `/regen-snapshots` if they were captured against the
  buggy kernels.
- **Neutral**: No change to CPU or SYCL paths; Netflix golden assertions
  are unaffected (CPU-only gate).

## References

- Round-6 scanner labels: `r6-cuda-kernel` × 2, `r6-hip-kernel` × 2
- `core/src/feature/cuda/integer_vif/filter1d.cu`
- `core/src/feature/cuda/integer_adm/adm_cm.cu`
- `core/src/feature/hip/integer_adm/adm_decouple.hip`
- `core/src/feature/hip/integer_vif/vif_statistics.hip`
