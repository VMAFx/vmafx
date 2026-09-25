<!-- markdownlint-disable MD013 MD060 -->
# ADR-1325: Normalize integer ADM Barten weights with one exponent per scale

- **Status**: Accepted; supersedes the finite-over-range rejection in [ADR-1191](1191-adm-csf-fixed-point-representability-guard.md)
- **Date**: 2026-09-25
- **Deciders**: VMAFx maintainers
- **Tags**: `metrics`, `adm`, `correctness`, `cuda`, `sycl`, `hip`, `metal`, `simd`

## Context

The public fixed-point `adm` extractor accepts `adm_csf_mode=1`, the Barten
contrast-sensitivity function. Its default scale factors produce CSF weights
from about 1.2 at scale 0 to 27 at scale 3. The original integer pipeline was
sized for Watson97 weights near 0.01: scale 0 stores Q21/Q23 weights in
`uint16_t`, while scales 1–3 store Q32 weights in `uint32_t` and feed signed
multiply/cube arithmetic.

The old narrowing casts wrapped those Barten weights and produced NaN or
near-zero output. ADR-1191 made that failure safe by rejecting weights that did
not fit, but it left a documented option unusable at its default value. The
open state row `T-ADM-CSF-MODE-1-BARTEN-DEGENERATE-2026-09-05` requires finite
CPU `adm2`, `aim`, and `adm3`, with the accelerator twins tracking the CPU at
places=4.

Simply widening `i_rfactor` is insufficient. Scale 0 deliberately narrows its
CSF bands to `int16_t`, the scale 1–3 filtering and masking paths use signed
intermediates, and the contrast-masking reduction cubes the weighted values.
Changing all those types would rewrite the Netflix-derived arithmetic and the
AVX2/AVX-512 implementations at once. We instead need a representation change
that preserves the mathematical weight and leaves the already-representable
path byte-identical.

[Research-2109](../research/2109-integer-adm-barten-fixed-point-normalization.md)
records the red reproducer, headroom experiments, float-reference comparison,
and backend verification.

## Decision

For each DWT scale, convert the three CSF bands to their existing fixed-point
format and choose the smallest non-negative integer exponent `k` for which all
three values fit the scale's arithmetic budget:

- scale 0: values are strictly below 2^16;
- scales 1–3: values are strictly below 2^30, retaining two headroom bits for
  the signed CSF/contrast-masking arithmetic and cube accumulation.

Divide all three bands on that scale by the same `2^k`. A shared exponent is
mandatory: scaling the horizontal, vertical, and diagonal bands independently
would change their CSF ratios and therefore define a different metric.

The contrast-masking accumulator contains the cube of the normalized signal.
Its host finalizer restores the removed scale by subtracting `3k` from the
existing power-of-two divisor exponent before applying the p-norm. The
denominator continues to use the original floating-point CSF factors. Invalid
negative or non-finite factors, including the blended-CSF tables' negative
sentinel for unsupported geometry, still fail with `-EINVAL`.

`core/src/feature/adm_csf_fixed_point.h` owns this conversion and exponent
contract. CPU, CUDA, SYCL, HIP, and Metal use it. CUDA and Metal apply the same
exponent to both DLM and AIM contrast-masking reductions. SYCL and HIP do not
claim AIM/ADM3 and apply it to their supported ADM2/per-scale outputs.

When every `k` is zero, the CPU keeps its existing AVX2/AVX-512 weighted CSF
and masking functions, so the default Watson97 path is unchanged. A
configuration needing normalization uses the scalar weighted-CSF and
contrast-masking stages while retaining SIMD DWT, decoupling, and denominator
work. Duplicating the new exponent through the hand-vectorized kernels is
deferred until the later performance phase; correctness is the RC1 priority.

Metal `integer_adm` now implements all four CSF modes and therefore removes
`VMAF_OPT_FLAG_DEFAULT_ONLY` from `adm_csf_mode`. The `float_adm` twins remain
Watson97-only and keep that capability restriction.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Widen every fixed-point band, product, SIMD lane, and accumulator | Avoids explicit scale metadata. | Rewrites the Netflix-derived scale-0 narrowing and every scalar/SIMD/device kernel; much larger numerical and overflow audit. | Disproportionate for the required mode; no benefit to already-representable configurations. |
| Keep ADR-1191's `-EINVAL` rejection | Smallest implementation. | Leaves a documented, range-validated default option unusable and the tracked pre-RC1 bug open. | Rejected: safe failure was explicitly an intermediate state. |
| Clamp or wrap each weight | No metadata or finalizer change. | Produces plausible but mathematically wrong scores. | Rejected: silent metric substitution is worse than failure. |
| Normalize each band independently | Maximizes precision in each integer. | Changes horizontal/vertical/diagonal ratios and therefore the metric. | Rejected: one exponent per scale is the invariant. |
| Use one exponent for all four scales | Simpler state. | The largest coarse-scale weight would discard needless precision at finer scales. | Rejected: per-scale exponents preserve more of the existing Q formats. |
| Port normalization into AVX2/AVX-512 immediately | Keeps all mode-1 weighted stages vectorized. | Expands the correctness fix into two hand-vectorized arithmetic rewrites before RC1. | Deferred to performance work after correctness and release cleanup. |

## Consequences

- **Positive**: `adm_csf_mode=1` at default scale now emits finite,
  non-degenerate `adm2`, `aim`, `adm3`, and all four scale scores. On the
  canonical 576x324 pair its seven pooled means differ from `float_adm` by at
  most `2.7e-5`.
- **Positive**: the shared helper keeps CPU and all accelerator twins on one
  representation contract, with places=4 parity tests for their supported
  outputs.
- **Positive**: Watson97 and already-representable Barten configurations use
  `k=0`, preserving their fixed-point values and existing SIMD dispatch.
- **Negative**: full-scale Barten uses scalar CPU CSF/CM stages until a later,
  separately measured SIMD port; no performance claim is made here.
- **Neutral / follow-ups**: `float_adm` remains the floating-point reference;
  SYCL/HIP AIM support remains tracked separately; no model, snapshot, Netflix
  golden assertion, tuning, benchmark, or retraining artifact changes.

## References

- [Research-2109](../research/2109-integer-adm-barten-fixed-point-normalization.md).
- [ADR-1191](1191-adm-csf-fixed-point-representability-guard.md),
  [ADR-0155](0155-adm-i4-rounding-deferred-netflix-955.md), and
  [ADR-0214](0214-gpu-parity-ci-gate.md).
- `docs/state.md` :: `T-ADM-CSF-MODE-1-BARTEN-DEGENERATE-2026-09-05`.
- `req`: "because we fix everything until we cant find anything anymore for now".
- `req`: "all bugs.md's in this local repo should of course be fully fixed".
