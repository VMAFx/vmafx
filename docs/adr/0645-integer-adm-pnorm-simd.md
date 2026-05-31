<!-- markdownlint-disable MD060 -->
# ADR-0645: Thread integer ADM p-norm through SIMD callbacks

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris maintainers
- **Tags**: simd, feature-extractor, testing

## Context

`integer_adm` exposes `adm_p_norm` so operators can tune the fixed-point
contrast-measure finalisation exponent with the same option shape as
`float_adm`. ADR-0623 deliberately landed the option before the full SIMD
plumbing and left an open backlog row: the AVX2 / AVX-512 contrast-measure
callbacks still accepted no p-norm parameter, so CPU SIMD dispatch continued
to use the default `3.0` exponent even when the caller set `adm_p_norm=2.0`.

The ADM SIMD paths are bit-exact twins of the scalar fixed-point kernel.
Changing their callback ABI is lower-risk than adding a parallel fallback
path because the default `3.0` arithmetic stays the same and the option value
is carried to the final p-norm exponent in one place.

## Decision

Thread `adm_p_norm` through the `adm_cm` and `i4_adm_cm` callback signatures
used by scalar, AVX2, and AVX-512 integer ADM, and compute the final
`powf(..., 1.0f / adm_p_norm)` terms from that parameter. Keep the default
`3.0` path unchanged for upstream/Netflix-compatible scoring and add an AVX2
unit test that proves both scale-0 and scale-1 CM callbacks respond to a
non-default p-norm.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave SIMD fixed at `3.0` and document the limitation | No code risk in an old upstream-mirror hot path | User-visible option remains misleading on common x86 builds | This preserves a known correctness gap instead of closing it |
| Disable SIMD when `adm_p_norm != 3.0` | Simple semantic guarantee | Surprising performance cliff; splits scalar/SIMD behavior by option value | The SIMD callbacks can carry the parameter directly |
| Add separate p-norm-specific callback variants | Keeps old ABI untouched | More dispatch state and duplicated code for no benefit | The existing callback signature is internal and easier to extend |

## Consequences

- **Positive**: `adm_p_norm` now reaches the x86 SIMD contrast-measure paths
  instead of being silently ignored.
- **Positive**: The default `adm_p_norm=3.0` path keeps the same expression
  shape, preserving Netflix-compatible default scoring.
- **Negative**: The internal ADM callback ABI changes, so scalar and both x86
  headers must move in lockstep on future rebases.
- **Neutral / follow-ups**: GPU integer-ADM backends still expose their own
  smaller option tables and keep their existing fixed exponent until a GPU
  parity PR explicitly adds `adm_p_norm` there.

## References

- [ADR-0623](0623-scaffold-audit-p2-half-finished.md)
- [docs/state.md](../state.md) — `T-INTEGER-ADM-P-NORM-SIMD-GAP`
- Source: `req` — user asked to continue with the next backlog.
