<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1207: A test gates every feature's score against the host instruction set

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: testing, simd, correctness, ci, reproducibility

## Context

The fork's contract is that every SIMD kernel is bit-exact with its scalar
reference, so a score must not depend on whether the host has AVX2, AVX-512,
NEON or nothing at all. Each extractor has a `test_<feature>_simd.c` asserting
that, and they all passed — yet `ssimulacra2` scored differently on a SIMD host
than on a scalar one for as long as ADR-1205 describes.

The reason is structural. The shipped scalar functions are `static`, so the
SIMD tests cannot call them; each test therefore defines its **own** scalar
reference and compares the SIMD kernel against that. When ADR-0891's FMA
unification was applied to the SIMD kernels and to the test's private
reference but not to the shipped scalar function, the test compared two things
that had both been updated, asserted bit-exactness, and passed. Nothing in the
suite compared the *shipped* SIMD path against the *shipped* scalar path.

`ADR-1208` is a second instance found the same way, so this is a recurring
class rather than a one-off.

## Decision

We will add `core/test/test_feature_isa_invariance.c`, which drives the public
API twice over one fixture — once with the host's real ISA and once with
`VmafConfiguration.cpumask` set to disable every SIMD flag, the same switch the
CLI exposes as `--cpumask` — and asserts the two scores are **bit-identical**
for every feature that has a SIMD path. It uses no internal symbols, so it
cannot drift away from the shipped code the way a private reference can.

The assertion is bit-identity, not a tolerance: this is one algorithm, one
implementation, one machine, and the fork's SIMD tests already claim
bit-exactness. A tolerance would hide exactly the defect class this exists to
catch.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Public-API round trip under two cpumasks (chosen) | Tests the shipped code on both sides; no internal access; one file covers every feature | End-to-end, so it localises a failure to a feature rather than a kernel | — |
| Un-`static` the scalar functions and have each SIMD test compare against them | Localises failures to the exact kernel | Widens the internal surface purely for tests, and each test still has to be remembered to update | Rejected — same maintenance hazard, larger blast radius |
| `#include "ssimulacra2.c"` into the test TU | Reaches the statics without changing the shipped surface | Per-extractor, fragile against build-flag differences between the test TU and the real TU — which is itself a way to reintroduce the bug | Rejected |
| Compare the shipped source text to the test reference in CI | Cheap | Textual, not behavioural; passes on code that is spelled differently but computes the same, and vice versa | Rejected |

## Consequences

- **Positive**: the invariant is now tested behaviourally. It caught ADR-1208
  on its first run. Nine of the ten features in the table already satisfied it,
  which is useful evidence in its own right.
- **Negative**: an end-to-end test localises to a feature, not a kernel. The
  per-feature `*_simd.c` tests remain the tool for pinpointing which kernel.
- **Neutral / follow-ups**: the fixture is 256x192 so that every feature in the
  table actually runs — `float_ms_ssim` rejects anything below 176 px
  (Netflix#1414 / ADR-0153) and 192 keeps the SSIM auto-scale at 1. Features
  whose only implementation is scalar are deliberately absent; adding one would
  assert nothing.

## References

- [ADR-1205](1205-ssimulacra2-fma-unification-scalar-and-gpu.md) — the defect
  that motivated this test.
- [ADR-1208](1208-ssimulacra2-edge-diff-double-subtract.md) — the defect this
  test found.
- [ADR-0891](0891-simd-bit-exact-round2-fmaf-libvmaf-feature-icx.md) — the
  bit-exactness contract being gated.
- Source: `req` — user direction to close the recurrence hole in the
  ssimulacra2 SIMD test.
