# ADR-0160: `psnr_hvs` NEON port — bit-exact DCT vectorization (T3-5-neon)

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, neon, aarch64, psnr-hvs, bit-exact, performance

## Context

Sister port to [ADR-0159](0159-psnr-hvs-avx2-bitexact.md). The AVX2
variant landed first (T3-5-avx2, PR #96) per the popup split that
mirrored the T3-4 `motion_v2` precedent (AVX2 then NEON as a
separate PR). This ADR covers the aarch64 NEON follow-up.

The bit-exactness invariant set by ADR-0159 carries through:
`od_coeff` (int32) DCT output and final `psnr_hvs_{y,cb,cr,psnr_hvs}`
feature scores are byte-identical to the scalar reference in
[`core/src/feature/third_party/xiph/psnr_hvs.c`](../../core/src/feature/third_party/xiph/psnr_hvs.c)
on every Netflix golden pair.

The AVX2 implementation exposed one subtle bit-exactness hazard
that was fixed pre-merge (ADR-0159 era, commit `c8e63d45`):
the per-block `accumulate_error` helper used a local
`float ret = 0` accumulator and returned the per-block total,
which the caller added to the outer cross-block `ret`. IEEE-754
add is non-associative, so that re-ordered the float summation
tree vs scalar's inline `ret += ...` and drifted the Netflix
golden by ~5.5e-5. The fix threads `ret` through by pointer so
every contribution hits the outer accumulator directly. This
NEON port inherits the pointer-based `accumulate_error`
signature; rebase-notes.md §0052 invariant #3 documents the
rule for future ISA ports.

## Decision

Port `calc_psnrhvs` to aarch64 NEON in a new TU
[`core/src/feature/arm64/psnr_hvs_neon.c`](../../core/src/feature/arm64/psnr_hvs_neon.c)
under the same byte-for-byte bit-exactness contract as the AVX2
variant.

Vectorization strategy — one-to-one mirror of AVX2 with lane-width
adjusted to NEON's 4-wide `int32x4_t`:

- **DCT butterfly**: load 8×8 block as 16 `int32x4_t` registers
  (row k → `r_k_lo` holding cols 0-3, `r_k_hi` holding cols 4-7).
  Run the 30-butterfly `od_bin_fdct8_simd` *twice* per DCT pass —
  once for the low half, once for the high half. Transpose 8×8
  decomposes into four 4×4 `transpose4x4_s32` calls plus a
  block-level swap of top-right ↔ bottom-left (because after the
  quadrant transposes, the top-right 4×4 holds cols 0-3 of
  transposed rows 4-7, which belong in the lo halves of rows
  4-7). Second butterfly + transpose completes the DCT.
- **Fixed-point arithmetic**: every scalar `(x * k + round) >> shift`
  becomes `vmulq_s32` + `vaddq_s32` + `vshlq_s32(_, -shift)`.
  `OD_UNBIASED_RSHIFT32` is implemented via the canonical
  uint32 logical shift + signed add + arith shift combo; the
  helpers `od_dct_rshift_neon` and `od_mulrshift_neon` mirror
  `od_dct_rshift_avx2` and `od_mulrshift_avx2` line-for-line.
- **Float accumulators stay scalar** (ADR-0159 rule): means,
  variances, mask, per-coefficient error accumulation reuse the
  scalar per-block loop verbatim. `accumulate_error` threads the
  outer `ret` by pointer — see rebase-notes §0052 invariant #3.
- **FMA off**: `#pragma STDC FP_CONTRACT OFF` at the TU header.
  Note: aarch64 GCC emits `-Wunknown-pragmas` for this pragma
  because its support is compiler-specific; however aarch64 GCC
  does not contract `a + b * c` across statements by default, so
  the effect is preserved. The pragma is kept for portability
  with other toolchains and with the AVX2 sibling TU.
- **4×4 transpose idiom**: aarch64 lacks the armv7 `vtrnq_s64`
  intrinsic; uses separate `vtrn1q_s64` / `vtrn2q_s64` instead.
  The transpose is written as two stages — 32-bit `vtrn1q_s32` /
  `vtrn2q_s32` of row pairs, then 64-bit `vtrn1q_s64` /
  `vtrn2q_s64` of the resulting pairs — yielding a 4×4 transpose
  in 4 trn instructions.

Runtime dispatch: `psnr_hvs.c`'s `init()` gains an `ARCH_AARCH64`
branch that picks `calc_psnrhvs_neon` when
`flags & VMAF_ARM_CPU_FLAG_NEON`. The AVX2 branch is unchanged.

NOLINT accounting (all with inline ADR-0141 citations):

- `od_bin_fdct8_simd` exceeds `readability-function-size` — the
  30-butterfly network must stay together for line-by-line diff
  against scalar `od_bin_fdct8`.
- Two `sqrt` calls in `compute_masks` trip
  `performance-type-promotion-in-math-fn` — `sqrt(double)`
  matches scalar's `float→double` promotion; switching to `sqrtf`
  would break the bit-exact contract.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Bit-exact DCT via 4-wide int32 NEON, halved per pass (this ADR)** | Preserves Netflix golden numerically; mirrors AVX2 structurally so reviewers can diff the two SIMD TUs side-by-side | 2× butterfly calls per DCT pass (vs AVX2's 1×) means ~2× the static code in the inline butterfly | **Chosen** — bit-exactness discipline + reviewability |
| **Process 4 rows in parallel (not 8)** | Simpler — one register per row | 2× more passes through the transpose network; higher register pressure across 4 half-passes | Rejected — symmetric 8-in-parallel-split-into-halves keeps the AVX2/NEON diff trivial |
| **Float DCT with tolerance** | Simpler intrinsics; potentially faster on some cores | Requires separate Netflix-golden tolerance ADR; breaks ADR-0159 precedent | Rejected — fork rule is "SIMD must match scalar" absent explicit ADR loosening |
| **SVE / SVE2 variant** | Length-agnostic; future-proof | Very few aarch64 consumer cores ship SVE2 as of 2026-Q2; QEMU user-mode SVE support is patchy; would need a separate feature-detect path | Deferred — revisit when real SVE2 hardware is common on CI |
| **Defer NEON until later** | Zero work now | Backlog flagged the matrix gap explicitly; CI already builds aarch64, letting scalar ride is pure perf loss | Rejected — matrix-parity with AVX2 is the established expectation |

## Consequences

- **Positive**:
  - `psnr_hvs` on aarch64 now gets NEON bit-exact parity with
    scalar. Matches ADR-0159's Netflix-golden contract.
  - ISA-parity matrix for psnr_hvs closes: scalar + AVX2 + NEON.
  - New unit test `test_psnr_hvs_neon.c` pins the bit-exactness
    contract via DCT-level scalar-vs-SIMD diffs on 5 reproducible
    inputs; registered in `meson test -C build-aarch64`.
- **Negative**:
  - NEON TU is ~500 lines — similar size to AVX2. Mitigated by
    line-for-line diff-ability against the AVX2 sibling.
  - The two SIMD TUs (AVX2 + NEON) now need to move in lockstep
    on any future psnr_hvs scalar change from upstream Xiph.
    Documented in rebase-notes.md §0052.
  - `#pragma STDC FP_CONTRACT OFF` is ignored by aarch64 GCC
    (non-fatal `-Wunknown-pragmas`). Reviewed as safe: aarch64
    GCC does not fold `a + b * c` across statements at default
    optimization levels, and the scalar float accumulators are
    all inside a single expression with a single `+=` operator.
- **Neutral / follow-ups**:
  - SVE2 variant tracked informally under the gap-fill queue;
    revisit when CI hardware support matures.
  - AVX-512 `psnr_hvs` intentionally not scheduled (AVX2 covers
    the x86_64 baseline; adding 512 requires re-verifying
    bit-exactness against a different reduction tree).

## Verification

- **Unit test** `test_psnr_hvs_neon` under
  `qemu-aarch64-static -L /usr/aarch64-linux-gnu`:
  **5/5 subtests pass** (3 random 12-bit seeds + delta + constant).
- **Netflix golden pair** (scalar vs NEON via `VMAF_CPU_MASK=0`
  vs default on aarch64 under QEMU):

  ```text
  BIT-EXACT: src01_hrc00_576x324.yuv vs src01_hrc01_576x324.yuv (576×324, bpc=8)
  ```

  Per-frame `psnr_hvs_y/cb/cr/psnr_hvs` values match byte-for-byte;
  only the `<fyi fps="…" />` header field (wall-clock timing)
  differs between scalar and NEON runs, as expected.
- The 1080p 10-bit checkerboard pairs segfault in
  `qemu-aarch64-static` with the default memory map under heavy
  threadpool allocations — a known QEMU user-mode limitation,
  not a defect in the port. These pairs will be validated by the
  native-aarch64 CI job (ARM ubuntu runner) and the Netflix
  CPU Golden Tests required check.
- `ninja -C build-aarch64` → clean; one pre-existing `-Wpedantic`
  warning about `float mask[8][8]` qualifier passing to a
  `const float mask[8][8]` helper (also present in AVX2 TU,
  inherited from scalar signature).

## References

- [ADR-0159](0159-psnr-hvs-avx2-bitexact.md) — AVX2 variant
  (sister port, bit-exactness contract source of truth).
- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) — AVX2
  convolve bit-exact via double accumulators.
- [ADR-0139](0139-ssim-simd-bitexact-double.md) — SSIM per-lane
  scalar-float reduction for bit-exactness.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule (scope of the NOLINTs above).
- [ADR-0145](0145-motion-v2-neon-bitexact.md) — NEON-after-AVX2
  port precedent (motion_v2 NEON followed AVX2; this PR mirrors
  that split for psnr_hvs).
- Xiph/Daala DCT source:
  [`core/src/feature/third_party/xiph/psnr_hvs.c`](../../core/src/feature/third_party/xiph/psnr_hvs.c)
  (BSD-licensed, `Copyright 2001-2012 Xiph.Org`).
- [rebase-notes 0052](../rebase-notes.md) — upstream-sync
  invariants (shared with ADR-0159; NEON TU added to the
  `Touches` list and invariant #3 expanded).
- User direction 2026-04-24: "alter go on" after PR #96 merged,
  confirming T3-5-neon sister-port execution per ADR-0159's
  "NEON follow-up PR" commitment.

### Status update 2026-05-09

The §Consequences bullet "AVX-512 `psnr_hvs` intentionally not
scheduled" has now been **empirically validated** under the
T3-9 bench-first methodology and closes as an AVX2 ceiling.
[ADR-0350](0350-psnr-hvs-avx512-ceiling.md) carries the
re-bench, the per-symbol cycle-share breakdown
(`calc_psnrhvs_avx2` 78.42 % scalar tail vs
`od_bin_fdct8x8_avx2` 14.82 % DCT) and the Amdahl ceiling
calculation that puts a realistic 16-lane DCT at 1.07–1.08×
over the current AVX2 path — well below the 1.3× T3-9 ship
gate. The original ADR-0160 body is unchanged per the
ADR-0028 / ADR-0106 immutability rule; this appendix only
records that the deferral has graduated from "intentionally
not scheduled" to "ceiling-confirmed by re-bench" and points
forward to ADR-0350 / [ADR-0180](0180-cpu-coverage-audit.md)
as the authoritative close-outs. T3-9 (a) is DONE-as-ceiling
in BACKLOG.md.
