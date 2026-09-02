<!-- markdownlint-disable MD013 MD060 -->
# ADR-0503: `vif_subsample_rd_8_avx512` Loop Fission to Reduce ZMM Register Spill

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `simd`, `performance`, `avx512`, `vif`

## Context

`vif_subsample_rd_8_avx512` is the hottest kernel in the integer VIF pipeline for
8-bit content. `perf annotate` identified a register-spill cluster consuming ~9% of
the function's cycles:

| Instruction | Hotness |
|---|---|
| `vmovdqa64 %zmm13, 0x480(%rsp)` | 4.47% |
| `vmovdqa64 %zmm7, 0x400(%rsp)` | 4.29% |
| `vmovdqa64 %zmm15, 0x4c0(%rsp)` | 1.10% |
| 5+ further spill sites | 1–2% each |

Root cause: the original monolithic function kept ~30 ZMM registers simultaneously
live — the vertical-pass pixel loads (g0-g9 = 10 ZMM, s0-s9 = 10 ZMM), the
per-pass filter constants (f0-f4, fcoeff-fcoeff4 = 10 ZMM), and the accumulator
and permutation constants (x, mask1, mask2, mask3, addnum = 5 ZMM). AVX-512 has
32 ZMM registers; the combined live-set exceeded what GCC's register allocator
could schedule without spilling.

The function is bit-exact with the scalar reference in `feature/integer_vif.c`
under ADR-0138 / ADR-0139. Any restructuring must preserve summation order.

## Decision

Extract the vertical-pass and horizontal-pass inner j-loop bodies into two
`static __attribute__((noinline))` helper functions (`vif_subsample_rd_8_vert_j`
and `vif_subsample_rd_8_horiz_j`). Filter constants are collected into two plain
structs (`VifVertCoeffs8`, `VifHorizCoeffs8`) allocated once on the caller's stack
and passed by pointer. `noinline` prevents the LTO pass from folding the helpers
back into the caller.

**Bit-exactness proof**: The accumulation order inside each helper is a verbatim
copy of the original loop body — no reordering of `_mm512_add_epi32` operands, no
change to shift constants, no floating-point operations. The only structural
difference is the ABI call/return boundary, which involves integer traffic only
and has no effect on the integer SIMD result. Verified by `meson test -C wt-build
--suite=fast` (50/50 including `test_vif_simd`) and the full Netflix golden gate
(`python/test/quality_runner_test.py` + `feature_extractor_test.py`, 132 passed,
0 failed).

**Spill reduction measured**: `objdump -d` on the output object shows 0 ZMM spill
stores (`vmovdqa64 %zmm*, <offset>(%rsp)`) across both helpers, down from 56 in the
original monolithic function.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **A. Loop fission via noinline helpers (chosen)** | Eliminates all 56 spills; zero mechanical change to accumulation order; verifiable via objdump | Adds two static helper symbols and two small structs | — |
| **B. `__attribute__((optimize("O2")))` on the whole function** | One-line change | Suppresses O3 for the entire function including non-spilling hot paths; compiler-specific; not portable to Clang | Too coarse, would regress other hot paths |
| **C. Manual nested block scopes** | No ABI boundary | Compiler is not required to honour scope hints; GCC O3 ignores them for live-range analysis | Unreliable; would require verifying effect per compiler version |

## Consequences

- **Positive**: ~9% spill-related cycle overhead eliminated in the inner loop; no
  regression in bit-exactness; no change to the external ABI of
  `vif_subsample_rd_8_avx512`.
- **Negative**: Two additional static symbols and two typedef-structs are added to
  the TU. `noinline` prevents LTO from further constant-propagating the filter
  values; any future decision to allow LTO inlining here would need to re-check
  register pressure.
- **Neutral**: `test_vif_simd` in `../../test/` already covers both helpers via
  the bit-exact harness (ADR-0245).

## References

- Performance profile: `perf_findings.md` Win #5, Approach A.
- ADR-0138 (`iqa-convolve-avx2-bitexact-double.md`) — bit-exactness contract for integer VIF.
- ADR-0139 (`ssim-simd-bitexact-double.md`) — per-lane scalar-double accumulate (VIF statistic path).
- ADR-0245 (`simd-bitexact-test-harness.md`) — shared bit-exact regression test harness.
- Source: req (user task brief, 2026-05-18).
