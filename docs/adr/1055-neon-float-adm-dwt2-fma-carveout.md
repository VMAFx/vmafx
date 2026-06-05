<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1055: NEON float_adm_dwt2 FMA carve-out — 1-ULP bit-exactness fix

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `simd`, `neon`, `build`, `bitexact`

## Context

PR #685 wired the float-ADM NEON kernels (`float_adm_dwt2_neon`,
`float_adm_csf_neon`, `float_adm_csf_den_scale_neon`,
`float_adm_sum_cube_neon`) into the runtime dispatch table for the first
time. Before that PR every `compute_adm()` call fell through to the scalar
path even on aarch64 hosts.

After wiring, `test_float_adm_dwt2_bitexact` failed on ARM64 CI with:

```text
dwt2 band_a[0][0]: scalar=0x1.6192dep+0 simd=0x1.6192dcp+0
```

A 1-ULP divergence. Root cause: the vectorized loop in
`float_adm_dwt2_neon` used `vmlaq_laneq_f32(acc, sN, filter, lane)` which
maps to the ARM `fmla v.4s[N]` instruction — a **single-rounding**
fused-multiply-add. The scalar reference (`adm_dwt2_s` in
`adm_tools.c`) computes the same sum as:

```c
accum = 0;
accum += filter[0] * s0;   /* two-rounding: mul then add */
accum += filter[1] * s1;
accum += filter[2] * s2;
accum += filter[3] * s3;
```

Each `accum +=` is two separate IEEE-754 operations (one multiply, one
add). The FMLA intrinsic combines them into a single rounding, producing a
different result by 1 ULP for certain input values.

The `arm64_v8_fp` static lib that compiles `float_adm_neon.c` already
carries `-ffp-contract=off` (ADR-0873), which correctly prevents the
compiler from auto-contracting scalar `a*b+c` expressions in the scalar
tail. However, `-ffp-contract=off` has no effect on the explicit
`vmlaq_laneq_f32` NEON intrinsic — it is always emitted as `fmla`
regardless of the FP-contract flag.

The same class of bug was addressed in PR #681 (ADR-0891) for the
ssimulacra2 AVX2/NEON paths via a combination of `#pragma STDC FP_CONTRACT
OFF` carve-outs and explicit FMA unification. For dwt2 the correct fix is
the opposite direction: use two-rounding mul+add (matching the scalar
reference) rather than FMA, because the reference is not feasibly ported
to FMA without modifying upstream Netflix code.

## Decision

In `float_adm_dwt2_neon`, replace the four `vmlaq_laneq_f32(acc, sN,
filter, lane)` calls (which emit FMLA) with explicit
`vaddq_f32(acc, vmulq_laneq_f32(sN, filter, lane))` (two separate
IEEE-754 operations: multiply first, then add).

Additionally, rewrite the scalar tail to use the same sequential
`accum = f[0]*s0; accum += f[1]*s1; ...` pattern as `adm_dwt2_s`, to
guarantee addition-order parity independent of compiler optimization
(the previous single-expression form `f[0]*s0 + f[1]*s1 + ...` could
associate differently under some optimization levels even with
`-ffp-contract=off`).

No meson build-system changes are needed: `float_adm_neon.c` remains in
`arm64_v8_fp` which already carries `-ffp-contract=off`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Unify on FMA (make scalar reference use fmaf) | Eliminates ULP gap, potential perf gain | Requires modifying upstream Netflix `adm_tools.c`; violates Netflix golden-assertion gate | Upstream code is frozen by ADR-0214 bit-exactness contract |
| `#pragma STDC FP_CONTRACT OFF` at file scope | Portable, already present in sister TUs | aarch64 GCC ignores this pragma for explicit NEON intrinsics (`vmlaq_laneq_f32` emits `fmla` regardless) | Does not fix the intrinsic-level FMLA emission |
| `__attribute__((target("+nofma")))` on function | Compiler won't emit FMLA for this TU | Non-portable; ARM-specific attribute; intrinsics still call hardware FMLA | Does not affect explicit NEON intrinsics |
| Explicit `vmulq_laneq_f32` + `vaddq_f32` (chosen) | Exact two-rounding semantics matching scalar; portable | Slightly lower throughput than FMLA (two microops vs one) | None — this is the correct approach |

## Consequences

- **Positive**: `test_float_adm_dwt2_bitexact` passes on ARM64. Scalar
  and NEON paths produce bit-identical output.
- **Negative**: The dwt2 vertical pass loses the throughput benefit of
  FMLA fusion (~2 cycles per stage on Cortex-A78 with `-mcpu=native`).
  In practice dwt2 is not the hottest path in float-ADM; the
  `float_adm_csf_den_scale` and `float_adm_sum_cube` kernels dominate.
- **Neutral / follow-ups**: The scalar tail was already protected by
  `-ffp-contract=off` in `arm64_v8_fp`; the extra rewrite is defensive
  but adds clarity. No snapshot regeneration required — the NEON path
  now produces the same values as the scalar reference, so no
  `testdata/scores_cpu_*.json` delta.

## References

- PR #685 — float-ADM SIMD dispatch wiring (introduced the regression).
- PR #681 / ADR-0891 — SIMD FMA unification cluster (prior art for this class of fix).
- ADR-0873 — arm64 float TU `-ffp-contract=off` policy.
- ADR-0214 — GPU/SIMD places=4 parity gate.
- `req`: User directive 2026-06-04 — fix the 1-ULP NEON dwt2 bit-exactness failure from PR #685.
