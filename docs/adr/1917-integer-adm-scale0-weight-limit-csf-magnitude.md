<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1917: The integer ADM scale-0 horizontal and vertical weight limit is 43900, set by the CSF stage's 16-bit magnitude

- **Status**: Accepted; narrows the scale-0 horizontal and vertical limit of [ADR-1472](1472-integer-adm-cm-weight-budget.md)
- **Date**: 2026-10-06
- **Deciders**: lusoris
- **Tags**: `metrics`, `adm`, `correctness`, `simd`, `cuda`, `sycl`, `hip`, `metal`, `rc3`, `fork-local`

## Context

ADR-1472 derived the scale-0 horizontal and vertical CSF weight limit from the
contrast-masking cube: 46603.4. The RC3 accumulator audit
(`docs/development/accumulator-bounds.md`) found two places where weights
below that limit still go wrong:

- the CSF stage stores the 1/30 magnitude of the weighted band,
  `(4369 * |i16| + 2048) >> 12`, in int16. It passes 32767 once `|i16|`
  reaches 30720, which for the largest scale-0 band the integer wavelet
  produces (22930) happens from a weight of 43900. The scalar code,
  AVX-512 and the GPU twins then wrap it negative, AVX2 saturates it, and the
  masking threshold built from it is wrong
  (`T-ADM-SCALE0-CSF-FLT-INT16-WRAP-2026-10-05`);
- a scale-0 masking row of a 31-32 pixel wide picture passes 2^64 from a
  weight of about 45200, even summed unsigned
  (`T-ADM-CM-SCALE0-ROW-UINT64-WEIGHT-BUDGET-2026-10-05`).

Watson97 at every viewing geometry, the default Barten configuration and both
blend modes stay below 43900. Weights in the window come from Barten mode with
a non-default `adm_csf_scale` or `adm_csf_diag_scale`.

## Decision

`adm_csf_fixed_limit(0, 0|1)` in `core/src/feature/adm_csf_fixed_point.h`
returns the smaller of the cube's limit and the CSF magnitude's, 43900,
derived from
`ADM_CSF_FLT_I16_LIMIT` (30720) and `ADM_DWT_BAND_REACH_SCALE0` (22930). The
shared normaliser `adm_csf_fixed_scale()` already halves every weight of a
scale until all are under their limits, so a weight in [43900, 46603.4) takes
one more halving instead of producing wrong scores. Every backend (CPU, AVX2,
AVX-512, CUDA, HIP, SYCL, Metal) takes its weights from that function, so no
kernel or constant elsewhere changes. At 43900 the largest scale-0 row is 0.912
of 2^64 (widths 28 to 32; `scripts/dev/adm_cm_row_bound.py`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Lower the limit to 43900 (chosen; maintainer's popup choice) | One constant in one header; every backend follows; defaults keep their bits; closes both rows | A Barten configuration in the window loses one bit of weight (about 1e-6 on the Netflix pair) | Chosen |
| Widen the CSF magnitude to int32 on every backend | Keeps every weight's precision | Changes the flt buffers and kernels of CPU, AVX2, AVX-512, CUDA, HIP, SYCL and Metal; leaves the 2^64 row open above 45200 | More code for a non-default range, and still needs a limit |
| Refuse a configuration whose weight lands in the window | Visible | Barten weights are always normalised (ADR-1325); refusing a weight the normaliser can bring under the limit would reject configurations that score correctly after one more halving | The normaliser is the existing mechanism |

## Consequences

- **Positive**: no scale-0 weight can wrap the CSF magnitude or pass 2^64 in
  a masking row; AVX2 and the scalar code agree again in the window.
- **Positive**: defaults are bit-identical: the Netflix golden gate passes and
  Watson97, the default Barten configuration and both blend modes give the same
  outputs as before on every backend.
- **Negative**: Barten configurations whose scale-0 weight lands in
  [43900, 46603.4) change in the sixth decimal place.
- **Neutral**: `core/test/test_integer_adm_cm_budget.c` derives both constants
  from the stage's arithmetic and the wavelet taps and fails under the old
  limit.

## References

- Q: "Lower limit to 43,900 (Recommended)" (maintainer popup, 2026-10-06, relayed by the orchestrator).
- `docs/development/accumulator-bounds.md` (RC3 accumulator audit), rows
  `T-ADM-SCALE0-CSF-FLT-INT16-WRAP-2026-10-05` and
  `T-ADM-CM-SCALE0-ROW-UINT64-WEIGHT-BUDGET-2026-10-05`.
- [ADR-1325](1325-integer-adm-barten-fixed-point-normalization.md), [ADR-1472](1472-integer-adm-cm-weight-budget.md).
