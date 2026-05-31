<!-- markdownlint-disable MD060 -->
# ADR-0973: Master CI fixes — Metal MS-SSIM fixture dim + ssimulacra2 icpx XYB bit-exactness

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: ci, simd, metal, ssimulacra2, icpx, bit-exactness, tests

## Context

Two CI regressions on `master` (tip `4948b771c`) blocked the merge train:

1. **macOS Metal MS-SSIM parity** (`test_metal_float_ms_ssim_parity`) failed
   on all three macOS jobs with `CPU: vmaf_read_pictures failed`. Root cause:
   the test fixture used `FIXTURE_W = 256u`, `FIXTURE_H = 144u`. The CPU
   `float_ms_ssim` extractor enforces a minimum input dimension at init
   (`core/src/feature/float_ms_ssim.c:131-138`):
   `min_dim = GAUSSIAN_LEN << (SCALES - 1) = 11 << 4 = 176`. `144 < 176`
   causes `init()` to return `-EINVAL`, which propagates to
   `vmaf_read_pictures` — the CPU twin failed before the Metal path ran.
   The macOS-only signal was misleading; the bug was unconditional, but the
   test was metal-gated so it only ran on Apple runners.

2. **Linux all-backends `test_ssimulacra2_simd::test_xyb`** failed with
   `linear_rgb_to_xyb SIMD not bit-identical to scalar`. Root cause: icx /
   icpx (Intel oneAPI 2025.3 in CI, reproduced on 2026.0.0 locally) emits
   `vfmadd231ps` instructions for the inline scalar reference
   `ref_linear_rgb_to_xyb`'s matrix-multiply chain
   (`kM00 * r + m01 * g + kM02 * b + kOpsinBias`) **despite** the test TU
   being compiled with both `-ffp-contract=off` AND `-fp-model=precise`.
   The AVX2/AVX-512 SIMD path uses explicit `_mm256_mul_ps` +
   `_mm256_add_ps` intrinsics (no `_mm256_fmadd_ps` in
   `linear_rgb_to_xyb_avx2`), so it emits separate `vmulps` + `vaddps`.
   Scalar contracted to FMA, SIMD did not — divergence by ~1 ULP per lane,
   `memcmp()` bit-exactness assertion failed.

Both failures were verified locally in the `vmaf-dev-mcp` container before
any code change. See companion research digest:
[`docs/research/0973-master-ci-regressions-verified-2026-05-31.md`](../research/0973-master-ci-regressions-verified-2026-05-31.md).

## Decision

We will:

1. Bump `FIXTURE_H` in `core/test/test_metal_float_ms_ssim_parity.c` from
   `144u` to `192u` (176 rounded up to a multiple of 16 for clean
   pyramid downsamples). Keep `FIXTURE_W = 256u` (already ≥ 176).

2. Add `#pragma clang fp contract(off)` (with a paired
   `-Wunknown-pragmas` suppression for GCC) at file scope in
   `core/test/test_ssimulacra2_simd.c`. This pragma is empirically the
   only mechanism that suppresses FMA contraction in icx 2025.3 /
   2026.0 — `-ffp-contract=off`, `-fp-model=precise`, and
   `#pragma STDC FP_CONTRACT OFF` are all ignored by icx for inline
   scalar code in this TU. icx is clang-based, so the clang FP pragma
   is honoured. GCC ignores the pragma (already had non-contracted code
   via `-ffp-contract=off`), with the warning suppressed.

The pragma is scoped to the test TU only. The production SIMD kernels and
the production scalar extractor are unchanged — there is no score drift.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Bump fixture to 256x192 in metal test** (chosen) | Trivial, mechanical, makes the test do what it always claimed to do (a 5-scale pyramid fixture) | None | Chosen |
| Lower `min_dim` enforcement in `float_ms_ssim.c` | None for the test | Breaks the Netflix#1414 invariant — small inputs would silently corrupt mid-pyramid | Rejected: load-bearing invariant |
| Use a different feature with no min-dim | None | Breaks the test's purpose (validates `float_ms_ssim_metal` specifically) | Rejected |
| **Add `#pragma clang fp contract(off)` to test TU** (chosen) | Surgical (one pragma at one file scope), zero impact on production code/scores, mirrors existing pattern in `core/src/feature/sycl/integer_adm_sycl.cpp`, passes on both GCC and icx | None observed | Chosen |
| Switch `_simd_strict_fp_args` to `-fp-model=strict` for icx | Also suppresses FMA in icx (verified empirically) | Broader semantics (denormals, exceptions) — risk of unrelated drift in other parts of the test TU; affects ALL SIMD tests, not just ssimulacra2 | Rejected: pragma is narrower |
| Switch AVX2/AVX-512 SIMD to `_mm*_fmadd_ps` (mirror ADR-0891 ptlr pattern) | Symmetric with existing ADR-0891 fix for `picture_to_linear_rgb` | Changes production SIMD score by ~1 ULP — risks breaking the snapshot gate and the cross-extractor parity invariant where GCC-compiled scalar `core/src/feature/ssimulacra2.c` (`-ffp-contract=off` default) does NOT use FMA | Rejected: introduces score drift |
| Rewrite scalar ref with `volatile` temporaries | Forces no contraction | Verbose, fragile, easy for a future contributor to "clean up" and reintroduce the bug | Rejected: pragma is self-documenting |
| Add `__attribute__((optnone))` to ref functions | Definitely disables FMA | Disables ALL optimisations, slows the test, leaves a foot-gun | Rejected |

## Consequences

- **Positive**: macOS Metal jobs go green again. Linux all-backends (icpx)
  job goes green again. No production code change, no score drift, no risk
  to the snapshot gates.
- **Negative**: Adds one more file with a `#pragma clang fp contract(off)`
  block; future contributors must know that the SSIMULACRA 2 test's scalar
  reference relies on this pragma when built with icx. This is documented
  inline at the top of the test file and recapped in `core/test/AGENTS.md`.
- **Neutral / follow-ups**: A future round-fix could also audit
  `core/src/feature/x86/ssimulacra2_host_avx2.c` and
  `core/src/feature/x86/ssimulacra2_avx512.c`, which use
  `#pragma STDC FP_CONTRACT OFF` (ignored by icx) for their scalar tail
  loops. The test-TU pragma alone resolves the *failing test*; those
  prod-side pragmas are vestigial but harmless on GCC and sit inside the
  strict-FP carve-out static lib (`-ffp-contract=off` + `-fp-model=precise`)
  whose intrinsics-only SIMD body is the actual binary surface against
  which the test compares. Auditing prod-side pragmas is out of scope here.

## References

- `core/src/feature/float_ms_ssim.c` lines 131-138 (min-dim gate, Netflix#1414).
- `core/test/test_float_ms_ssim_min_dim.c` (existing test proving the 176 floor).
- `core/src/feature/x86/ssimulacra2_avx2.c::ssimulacra2_linear_rgb_to_xyb_avx2`
  (uses explicit `_mm256_mul_ps` + `_mm256_add_ps`, no FMA intrinsics).
- `core/test/meson.build` lines 25-39, 598-622 (the `_simd_strict_fp_args` +
  `_ssimulacra2_test_x86_fma_args` regime that this fix supplements).
- ADR-0153 (Netflix#1414 — `float_ms_ssim` min-dim init check).
- ADR-0161 / ADR-0162 / ADR-0163 (SSIMULACRA 2 SIMD bit-exactness contract).
- ADR-0214 (cross-backend parity gate).
- ADR-0589 (Metal SSIM L/C/S parity bound).
- ADR-0891 (explicit `fmaf()` unification in `picture_to_linear_rgb`).
- Source: per user direction — `req` ("Fix 2 verified master CI regressions.
  HARD REQUIREMENT: reproduce EACH failure locally in the `vmaf-dev-mcp`
  container BEFORE writing any fix. No guessing.")
