<!-- markdownlint-disable MD013 -->

# ADR-1194: One integer-ADM `angle_flag` predicate for every backend

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cuda, hip, sycl, metal, simd, correctness

## Context

`decouple()` in integer ADM asks whether the reference and distorted DWT
H/V vectors are within one degree of each other:

```text
{ u.v >= 0 } AND { (u.v)^2 >= cos(1deg)^2 * ||u||^2 * ||v||^2 }
```

The flag selects the enhancement-gain-limited branch of `decouple()`, so
flipping it moves the `adm` scores.

Upstream evaluates the test in floating point *after narrowing the exact
int64 operands to `float`*. That narrowing is lossy past the 24-bit binary32
significand, so the shipped predicate is not the mathematically exact angle
test — but it is the expression the Netflix golden-data gate freezes
(CLAUDE.md rule 1), and the fork cannot change it.

The fork had drifted into **four** different spellings of that one test
(T-UPSTREAM-930, `docs/state.md`):

| Site | Evaluation |
| --- | --- |
| `integer_adm.c`, `adm_avx2.c`, `adm_avx512.c` | narrow to float, compare in binary64 — the golden form |
| CUDA / HIP `decouple_angle_flag_s0` | compare the *exact* int64 products in binary64 |
| CUDA / HIP `decouple_angle_flag_s123` | the golden form (so `s0` and `s123` disagreed inside one backend) |
| SYCL `integer_adm_sycl.cpp` (both scales) | the whole comparison in binary32 |
| Metal `iadm_angle_flag_s0` | exact int64 products narrowed to binary32 |

A 3 M-sample sweep of near-parallel int16 band quadruples puts the
disagreement with the golden form at 117 (CUDA/HIP `s0`), 123 (SYCL) and
158 (Metal) — around 4e-5 of scale-0 pixels, concentrated exactly on the
`cos(1deg)^2` boundary once `|band|` runs past 2^24.

The obvious fix — make every backend evaluate the golden expression — runs
into a hardware constraint. `integer_adm_sycl.cpp` deliberately contains no
binary64 operation at all: Intel Arc A-series and most integrated GPUs
expose no fp64, and a single fp64 instruction anywhere in a SYCL translation
unit makes the runtime reject the whole SPIR-V module, even for kernels that
never run it. Metal Shading Language has no `double` type in the first
place. So two of the four backends cannot execute the golden expression as
written.

## Decision

We will keep exactly one `angle_flag` predicate, in
`core/src/feature/adm_angle_flag.h`, expressed twice:

- `adm_angle_flag_fp64()` — the golden expression, verbatim. The scalar CPU
  path, CUDA and HIP call it (both scales).
- `adm_angle_flag_i64()` — a reformulation in 64-bit integer arithmetic that
  returns the **bit-identical** result using no floating-point operation of
  any width. SYCL calls it; `core/src/feature/metal/integer_adm.metal`
  mirrors it in MSL.

The reformulation works because every quantity in the golden expression is a
scaled integer. With `of = (float)ot_dp = mp*2^ep` (and likewise `mo`, `mt`),
24-bit significands, and `c = (float)cos(1deg)^2 = MC * 2^-24` exactly
(`MC = 16772106`):

```text
LHS = (of/4096)^2                    = mp^2 * 2^(2*ep-24)      exact in f64
RHS = fl( (c*(om/4096)) * (tm/4096) )= round53(MC*mo*mt*2^-24) * 2^(eo+et-24)
```

so the predicate is the integer comparison `mp^2 * 2^(2*ep-eo-et) >=
round53(V)` with `V = MC*mo*mt*2^-24`. Writing `V = S - D*r*2^-24` with
`D = 2^24 - MC = 5110` keeps every intermediate inside 64 bits — no 128-bit
product, no floating point. The derivation, the bounds proof and the
verification runs are in
[`docs/research/2030-adm-angle-flag-fp64-free.md`](../research/2030-adm-angle-flag-fp64-free.md).

`core/test/test_adm_angle_flag.c` pins the two against each other and against
the golden expression, so the CPU path stays the reference and no backend can
drift away from it again.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Chosen** — shared header, fp64 form on CPU/CUDA/HIP, bit-identical int64 form on SYCL/Metal | every backend evaluates one predicate; no fp64 requirement; provable, host-testable | the int64 form is ~40 lines of rounding-aware integer code and is duplicated once in MSL | — |
| Make every backend evaluate the exact int64 angle test (i.e. adopt the *more accurate* CUDA `s0` form) | mathematically correct angle test; simpler code | changes CPU scores, which the Netflix golden gate freezes (CLAUDE.md rule 1) | not available: the CPU expression cannot move |
| Golden expression everywhere, in binary64 | trivially correct; five-line diff | breaks SYCL on every non-fp64 device (whole SPIR-V module rejected) and does not compile in MSL at all | the two fp64-free backends are the ones that need the fix most |
| Leave SYCL/Metal on their float forms, fix only CUDA/HIP | smallest diff | leaves two backends knowingly divergent; the ledger row could not close | half a fix |
| Compensated binary32 arithmetic (2Product via `fma`) on SYCL/Metal | stays in float; fast | reproducing binary64's single rounding needs a 4-term expansion comparison — more subtle than the integer form, and still not exact without modelling `round53` | integer form is exact and easier to reason about |
| Accept the exact real comparison on SYCL/Metal and skip the `round53` modelling | ~10 lines shorter | leaves a residual divergence in a 2^-53-relative window around equality: unreachable in practice, but "unreachable in practice" is what produced this bug | the whole point is to remove the caveat |

## Consequences

- **Positive**: one predicate, one test, one place to change. SYCL and Metal
  now agree with the golden CPU expression bit-for-bit on the angle test,
  which was previously impossible for them; CUDA/HIP `s0` and `s123` no
  longer disagree with each other. The SYCL kernel keeps its fp64-free
  property, so it still runs on Arc A-series and iGPUs.
- **Negative**: the fork now deliberately reproduces a *less* accurate angle
  test on GPUs than the hardware could compute. That is the price of golden
  parity and is called out at every call site. The MSL mirror in
  `integer_adm.metal` is a hand-copy of the C header (MSL cannot include it),
  so the two must be edited together — noted in
  `core/src/feature/AGENTS.md`.
- **Neutral / follow-ups**: GPU `adm` scores move on the affected pixels, so
  the fork-added GPU snapshots under `testdata/` are regenerated in the same
  PR. The CPU golden assertions are untouched and must stay green — they are
  the gate that proves the shared header still spells the frozen expression.
  The AVX-512 vectorised `s0` path multiplies its binary64 operands in a
  different association than the scalar path (`(om*tm)*c` instead of
  `(c*om)*tm`); that is a last-ULP difference inside the golden lane, out of
  scope here, and recorded in the research digest.

## References

- `docs/state.md` — T-UPSTREAM-930-ADM-ANGLE-FLAG-PREDICATE-DIVERGENCE-2026-09-03.
- [Netflix/vmaf#930](https://github.com/Netflix/vmaf/issues/930).
- [`docs/research/2030-adm-angle-flag-fp64-free.md`](../research/2030-adm-angle-flag-fp64-free.md) — derivation and verification.
- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md), [ADR-0139](0139-ssim-simd-bitexact-double.md) — the bit-exactness precedents this follows.
- CLAUDE.md rule 1 — Netflix golden assertions are frozen.
