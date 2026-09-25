<!-- markdownlint-disable MD013 MD060 -->
# ADR-1326: Use a fixed-point oracle for SYCL motion-add-UV parity

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `sycl`, `motion`, `correctness`, `testing`, `numerics`

## Context

`test_sycl_motion_add_uv_parity` compared two implementations of the same
semantic option but different arithmetic. The CPU `float_motion` extractor
uses decimal float coefficients and float row/SAD reductions. The SYCL
`motion_sycl` extractor uses Q8 intermediate values, integer coefficients that
sum to 65536, a rounding step after each separable pass, exact `int64_t` SAD
accumulation, and host double normalization.

The original `2e-4` absolute tolerance was calibrated against one 256x144
fixture. At 960x540 on the Arc A380, CPU float scored `50.27938271` and SYCL
fixed-point scored `50.27915223`, a `2.30e-4` difference. Raising the constant
would make that fixture green without proving that the device executed the
fixed-point algorithm correctly, and the large-fixture test therefore remained
unregistered under ADR-1206.

## Decision

Replace the CPU-float numerical comparison with a scalar fixed-point oracle in
`test_sycl_motion_add_uv_parity.c`. The oracle independently reconstructs:

1. the five integer coefficients `{3571, 16004, 26386, 16004, 3571}`;
2. reflect-101 border indices;
3. vertical `(sum + 128) >> 8` and horizontal `(sum + 32768) >> 16` rounding;
4. exact `int64_t` SAD accumulation for consecutive frames; and
5. Y, U and V normalization by `256 * plane_area`.

Assert both the Y-only score and the Y+U+V score against that oracle. For every
8-bit picture accepted by the 32768-per-side allocator limit, the raw SAD
remains below `2^53`, so its conversion to double is exact. Division by 256 is
exact in binary. The three area divisions and two additions are the only
rounded host operations in the combined score. The comparison budget is

```text
2 * gamma_5 * max(1, |expected|)
gamma_5 = (5u) / (1 - 5u),  u = DBL_EPSILON / 2
```

where the factor two permits independent host evaluations. This normalized
bound is valid at every resolution; it is not calibrated from either fixture.
Register the existing 960x540 build variant in
`sycl_parity_large_fixture_tests`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `2e-4` and leave the large fixture unregistered | No test churn | Leaves the known resolution blind spot and does not prove kernel arithmetic | Rejected: the open state row remains real |
| Raise the float-vs-fixed tolerance to `2.5e-4` | Makes both current fixtures pass | Fits observations rather than deriving a contract; still compares unlike arithmetic | Rejected: test weakening |
| Derive a worst-case float-vs-fixed envelope | Retains a cross-extractor comparison | Float coefficient, convolution, and reduction roundoff make the envelope much looser than the device invariant | Rejected as the primary gate |
| Compare raw fixed arithmetic through a scalar oracle | Resolution-independent and sensitive to coefficient, border, rounding, plane, and normalization drift | Duplicates a small test-only arithmetic model | **Chosen** |

## Consequences

- **Positive**: Both small and large fixtures now enforce the actual device
  arithmetic under a mathematically derived bound.
- **Positive**: A one-unit mutation of the first filter coefficient moves the
  960x540 Y+U+V score by `3.291e-3` and fails the new gate, while the former
  float tolerance could not identify which implementation moved.
- **Negative**: The test duplicates the fixed filter and rounding sequence;
  changes to the intended algorithm must update production and oracle together
  with explicit review.
- **Neutral**: No production code, model, snapshot, public header, CLI,
  FFmpeg patch, Netflix golden assertion, benchmark, tuning, or training path
  changes.

## References

- [ADR-0989](0989-sycl-motion-add-uv.md) — SYCL implementation and semantic option contract.
- [ADR-1206](1206-gpu-parity-large-fixture-variants.md) — large-fixture parity variants and the former exclusion.
- [Research-2112](../research/2112-sycl-motion-uv-fixed-oracle-2026-09-25.md) — derivation, reproduction, red cap, and hardware evidence.
- `T-SYCL-MOTION-ADD-UV-TOLERANCE-RESOLUTION-2026-09-06` in `docs/state.md`.
- Source: `req` — “we fix everything until we cant find anything anymore for now”.
