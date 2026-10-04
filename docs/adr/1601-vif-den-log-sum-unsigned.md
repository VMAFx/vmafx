<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1601: integer VIF forms the denominator log argument `sigma_nsq + sigma1_sq` in `uint32_t`

- **Status**: Accepted
- **Date**: 2026-10-04
- **Deciders**: lusoris
- **Tags**: `numerics`, `vif`, `simd`, `cuda`, `hip`, `metal`, `aarch64`, `upstream-parity`, `rc3`, `fork-local`

## Context

In the logarithm branch Netflix's `integer_vif.c` passes `sigma_nsq + sigma1_sq`
to `log2_32()`, whose parameter is `uint32_t`. Both operands are `int32_t`, so
the addition is signed, and `sigma1_sq` above `INT32_MAX - 131072` overflows it
(undefined, N1570 6.5p5). UBSan stops on it
(`131072 + 2147441255`, `test_integer_vif_sv_sq`). The same statement is in the
AVX2 and NEON paths and in the CUDA, HIP and Metal kernels
([ADR-1561](1561-integer-vif-sv-sq-defined.md) covers the residual variance
and the `sv_sq + sigma_nsq` sum, not this one).

## Decision

Every copy forms the argument as `(uint32_t)sigma_nsq + (uint32_t)sigma1_sq`.
For every input the signed sum did not overflow on, the result is the same
number; for the rest, it is the value the two's-complement wrap gave on every
target the fork builds for, now defined. No output moves, so the upstream
parity guard reports no difference and no allowlist fragment is added. The SYCL
twin already adds in `int64_t`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave Netflix's signed sum | upstream-identical text | undefined; fails the sanitizer job | the sanitizer gate must pass |
| Widen to `int64_t` then narrow | defined | adds a conversion on the hot path in four copies | the unsigned add is one instruction |

## Consequences

- **Positive**: UBSan-clean on the largest variances; one form on every path.
- **Negative**: a sync that takes Netflix's text must keep this form.
- **Neutral / follow-ups**: `docs/rebase-notes.md` entry; `test_integer_vif_sv_sq` covers it under UBSan.

## References

- Source: coordinator brief of 2026-10-04 (paraphrased): a change to
  Netflix-inherited arithmetic needs an ADR; claim one if ADR-1561 does not
  cover the statement.
- ISO/IEC 9899:201x committee draft N1570, 6.5p5.
