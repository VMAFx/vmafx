<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1174: Reject Integer-ADM CSF Modes Exceeding 16-bit Scale-0 Fixed-Point Budget

- **Status**: Accepted
- **Date**: 2026-09-04
- **Deciders**: Kilian, Lusoris maintainers
- **Tags**: feature, adm, integer-adm, csf, overflow, security

## Context

The `integer_adm` feature extractor operates on a 16-bit fixed-point arithmetic pipeline
where scale-0 Contrast Sensitivity Function (CSF) factors are scaled by $2^{21}$ for horizontal
and vertical subbands and by $2^{23}$ for diagonal subbands, stored in a 3-element `uint16_t i_rfactor`
array (`core/src/feature/integer_adm.c`).

Under default settings (`adm_csf_mode=0` / `ADM_CSF_MODE_WATSON97`, `adm_norm_view_dist=3.0`,
`adm_ref_display_height=1080`), `i_rfactor` evaluates to `{36453, 36453, 49417}`, which safely fits
within `uint16_t` (0–65535).

However, fork-added alternative CSF models (`adm_csf_mode` 1..3: Barten, Barten-Watson blend,
Barten-Watson blend MAE) dynamically compute scale-0 factors. Under `adm_csf_mode=1` (`BARTEN`) at
standard 1080p/3H viewing distance, the unquantized CSF factors are $\approx 1.21$ on H/V and
$\approx 1.21$ on D. Scaled by $2^{21}$ ($2\,538\,596$) and $2^{23}$ ($10\,154\,382$), these factors
severely exceed the 65535 maximum of `uint16_t`, wrapping modulo $2^{16}$ to $48\,227$ and $61\,838$
(52× and 164× attenuation). This causes massive silent score distortion (~1500× discrepancy on scale-0
scores relative to floating-point ADM).

While stage 2 will widen the internal pipeline (`i_rfactor` to `uint32_t`, 64-bit SIMD arithmetic),
an immediate defensive gate is required in stage 1 to prevent silent emission of numerical nonsense.

## Decision

We will validate the scale-0 CSF factors in `integer_adm.c:extract()` adjacent to the existing
`adm_norm_view_dist * adm_ref_display_height` check. If `(double)factor1 * 2^21 >= 65536.0` or
`(double)factor2 * 2^23 >= 65536.0`, the extractor logs an informative error message via
`vmaf_log(VMAF_LOG_LEVEL_ERROR, ...)` naming the mode and calculated factors, and returns `-EINVAL`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Option A: Immediate pipeline widening | Eliminates limitation in a single step | High complexity: requires widening `adm_cm` products, re-deriving `ADM_CM_ACCUM_ROUND` shift schedules, and rewriting AVX2/AVX-512 SIMD kernels to 64-bit lanes | High regression risk; better handled in a dedicated stage 2 refactor |
| Option B: Saturate / clamp to 65535 | Avoids error exit | Distorts the frequency response curve without notifying the user; still outputs inaccurate scores | Silent inaccuracy violates numerical truthfulness |
| Option C: Defensive input rejection in stage 1, widening in stage 2 (Chosen) | Immediate safety; turns silent numerical corruption into a loud, clear `-EINVAL`; zero risk to existing default path | Rejects `csf_mode=1` under default display geometry until stage 2 lands | Safest path forward; users requiring Barten CSF can use `float_adm` in the interim |

## Consequences

- **Positive**: Configurations that would overflow the 16-bit budget fail fast with `-EINVAL` and an explicit error log instead of emitting corrupt scores.
- **Negative**: `adm_csf_mode=1` cannot be evaluated via `integer_adm` at 1080p/3H until stage 2 widening lands.
- **Neutral / follow-ups**: Stage 2 pipeline widening is tracked under `T-ADM-CSF-IRFACTOR-WIDEN`. The rejection is documented in `docs/metrics/features.md`.

## References

- T-UPSTREAM-1494-ADM-CSF-MODE-IRFACTOR-OVERFLOW-2026-09-03
- [ADR-0155](0155-adm-i4-rounding-deferred-netflix-955.md)
- [ADR-0535](0535-adr-atomic-allocator.md)
- Source: T-UPSTREAM-1494 residual tracking
