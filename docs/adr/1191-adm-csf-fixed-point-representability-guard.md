<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1191: Integer ADM rejects CSF configurations its fixed-point storage cannot represent

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: `metrics`, `adm`, `cuda`, `sycl`, `hip`, `correctness`

## Context

The integer ADM pipeline stores the contrast-sensitivity (CSF) weight of each
DWT scale as a fixed-point integer: `uint16_t i_rfactor[3]` at scale 0 (the
16-bit pipeline; horizontal/vertical bands scaled by 2^21, diagonal by 2^23)
and `uint32_t i_rfactor[3]` at scales 1-3 (all bands scaled by 2^32). Those
budgets were sized for the Watson97 CSF, whose weights sit around 1e-2.

The fork-added `adm_csf_mode` option exposes three more models. Two of them
produce weights the storage cannot hold:

- **Barten (`adm_csf_mode=1`)** at the default `adm_csf_scale=1.0` returns
  1.2105 at scale 0 and 26.98 at scale 3. The scale-0 conversions are
  2 538 595 and 10 154 382 -- 38x and 155x past 65 535 -- and every scale-1..3
  conversion is past 2^32 as well. The narrowing casts wrapped silently.
- **The blended-CSF tables (`adm_csf_mode=2` / `=3`)** return `-EINVAL` *as a
  float* (i.e. `-22.0f`) for any `(adm_norm_view_dist, adm_ref_display_height)`
  pair they do not tabulate. Converting a negative float to an unsigned
  integer type is undefined behaviour (C17 6.3.1.4p1). `adm_ref_display_height`
  values such as 1200 clear the pre-existing `nvd * rdh >= 3240` guard and
  reach that cast.

Neither produced an error. Scoring the 576x324 Netflix fixture pair with
`--feature adm=adm_csf_mode=1` emitted `integer_adm2_csf_1: null` (NaN),
`integer_adm_scale0_csf_1: 0.030096` and `integer_adm_scale2_csf_1: 0.00032`
against the fork's own float reference of `0.9396` -- silently wrong numbers
for a documented, range-validated public option. Tracked as
`T-UPSTREAM-1494-ADM-CSF-MODE-IRFACTOR-OVERFLOW-2026-09-03` (and, from the
GPU-parity side, `T-ADM-CSF-MODE-1-BARTEN-DEGENERATE-2026-09-05`).

Widening the storage is not a local change: the scale-0 CSF output is written
into `int16_t` bands, so a 70x larger weight overflows the *next* stage too,
and the `ADM_CM_ACCUM_ROUND` shift contract and the AVX2 / AVX-512 32-bit-lane
twins would all have to be re-derived. That work is out of scope here; what is
in scope is that wrong numbers must not be emitted in the meantime.

Per SEI CERT INT31-C and FLP34-C, a conversion that cannot represent its
operand is a defect, not a rounding mode; per NASA/JPL Power of 10 rule 7,
the configuration has to be validated before it is used.

## Decision

We will validate the configured CSF weights against the fixed-point storage
that will hold them, and refuse the configuration with `-EINVAL` when they do
not fit. `core/src/feature/adm_csf_fixed_point.h` owns the bounds, the
tabulated-fast-path predicate, and the narrowing conversion; `init()`
evaluates the verdict once per extractor context (the weights cannot change
after option parsing) and `extract()` returns it, beside the pre-existing
viewing-geometry guard. The CUDA, HIP and SYCL twins apply the identical
bounds from the same header so their accept/reject set matches the CPU
reference exactly.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Widen `i_rfactor` to `uint32_t` / `int64_t` products now | Actually supports Barten in the integer pipeline. | The scale-0 CSF output lands in `int16_t` bands, so widening the weight alone still overflows the next stage; `ADM_CM_ACCUM_ROUND` and the AVX2 / AVX-512 32-bit-lane twins need re-derivation, and there is no reference to validate the new Q format against. | Rejected *for this change*, not forever: it is the remaining half of the ledger row and stays open. Emitting `-EINVAL` is a correct intermediate state; emitting NaN is not. |
| Narrow the `adm_csf_mode` range to `0,2,3` on the integer extractor | One-line change. | Removes a user surface, and mode 1 *is* usable with small `adm_csf_scale` values -- `python/test/feature_extractor_test.py` pins integer scores for `adm_csf_mode=1` with `adm_csf_scale=0.002893`. A range narrowing would break those. | Rejected: the defect is representability of the *weights*, not of the mode. |
| Clamp the converted weight to the storage maximum | Always produces a number. | Saturating is as wrong as wrapping, and quieter: the score would look plausible while the CSF model was silently replaced. | Rejected. Fail loudly, never plausibly. |
| Check per-frame in `extract()` without caching | No new state field. | `adm_csf_factors()` runs `pow()` / `log10()` for four scales; the answer is fixed after option parsing. | Rejected as needless per-frame work; the verdict is cached in `AdmState`. |
| Fail in `init()` instead of `extract()` | Fails before buffers are allocated. | Relocates the established contract: `core/test/test_adm_coverage.c::test_adm_invalid_view_dist_returns_einval` pins that an unsupported ADM configuration initialises and then fails at `extract()`. | Rejected on the CPU twin to leave that contract (and its test) untouched. The GPU twins already reject the viewing geometry in their own `init()`, so they keep that idiom. |

## Consequences

- **Positive**: `--feature adm=adm_csf_mode=1` now prints which scale, band and
  weight overflowed and returns `-EINVAL`, instead of emitting NaN and
  three-orders-of-magnitude-wrong scale scores. The undefined negative-to-
  unsigned conversion on untabulated blend geometries is gone. CPU, CUDA, HIP
  and SYCL accept exactly the same configurations, which the ADR-1183 option /
  feature-name parity contract depends on.
- **Negative**: a configuration that previously "ran" now errors. It never
  produced usable numbers, so no result is lost -- but any script that scraped
  `integer_adm*_csf_1` values will now see a failed run instead of nonsense.
  Callers that want the Barten CSF at full scale must use `float_adm`, which
  has no fixed-point limit.
- **Neutral / follow-ups**: the widening half of
  `T-UPSTREAM-1494-ADM-CSF-MODE-IRFACTOR-OVERFLOW-2026-09-03` and all of
  `T-ADM-CSF-MODE-1-BARTEN-DEGENERATE-2026-09-05` stay open. The AVX2 /
  AVX-512 ADM kernels keep their own copies of the scale-0 conversion; they
  are unreachable with an out-of-range weight now that `init()` gates the
  configuration, so they were left byte-identical to keep the SIMD
  bit-exactness story unchanged.

## References

- `docs/state.md` :: `T-UPSTREAM-1494-ADM-CSF-MODE-IRFACTOR-OVERFLOW-2026-09-03`,
  `T-ADM-CSF-MODE-1-BARTEN-DEGENERATE-2026-09-05`.
- Netflix/vmaf#1494 (the upstream report; its `nvd`/`rdh` half does not affect
  this fork -- see the "Confirmed not-affected" row in `docs/state.md`).
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) -- the option /
  feature-name parity contract the twins must not break.
- [ADR-0141](0141-touched-file-cleanup-rule.md), [ADR-0165](0165-state-md-bug-tracking.md).
- SEI CERT `INT31-C`, `FLP34-C`; NASA/JPL Power of 10 rule 7.
- `core/test/test_adm_csf_representable.c` (the regression assertions).
