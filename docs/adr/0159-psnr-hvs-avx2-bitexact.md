# ADR-0159: `psnr_hvs` AVX2 port — bit-exact DCT vectorization (T3-5)

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, avx2, psnr-hvs, bit-exact, performance

## Context

Backlog item T3-5 called for an AVX2 + NEON port of
[`psnr_hvs`](../../core/src/feature/third_party/xiph/psnr_hvs.c)
— a perceptual PSNR variant based on Xiph/Daala's 8×8 integer DCT
with contrast-sensitivity weighting and masking. Per user popup
2026-04-24, the scope is split:
**this ADR covers AVX2; NEON follows as a sister PR**, matching the
T3-4 `motion_v2` NEON-after-AVX2 precedent.

Scalar hot path (409 lines, `static double calc_psnrhvs()`):
for every `_step`-strided overlapping 8×8 block, the extractor

1. packs 8×8 pixels into `od_coeff[64]` (int32, supports 8–12 bpc);
2. computes the global mean + 4 quadrant sub-means;
3. computes the global variance + 4 quadrant variances;
4. calls `od_bin_fdct8x8` on ref + dist — a 30-butterfly 8-point
   integer DCT applied as `od_bin_fdct8` per row, then per column;
5. builds `s_mask` / `d_mask` from `sum(dct² × mask²)` over 63 AC
   coefficients;
6. accumulates `(|dct_s − dct_d| − threshold)² × csf²` into the
   per-plane score.

Called 3× per frame (one per YUV plane). At 1080p with `_step=7` the
extractor dispatches ~160 k 8×8 DCTs per plane per frame. The DCT
is the hot kernel by a wide margin.

The DCT does not vectorize within a single row — the butterfly
dependency chain is serial — but it vectorizes **beautifully across
8 rows at once**: 8 rows × int32 = one `__m256i` register wide, and
each scalar butterfly line becomes one AVX2 op on the whole vector.

Fork precedent for "bit-exact SIMD under `FLT_EVAL_METHOD == 0`" is
[ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) (convolve
double-accumulator) and
[ADR-0139](0139-ssim-simd-bitexact-double.md) (SSIM per-lane
scalar-float reduction). Those set the bar: every SIMD variant
produces byte-identical output to scalar on the Netflix golden pairs.
This ADR re-applies the same rule to psnr_hvs.

## Decision

Port `calc_psnrhvs` to AVX2 in a new TU
[`core/src/feature/x86/psnr_hvs_avx2.c`](../../core/src/feature/x86/psnr_hvs_avx2.c)
under the constraint that **every `od_coeff` emitted by the DCT and
every final `psnr_hvs_{y,cb,cr,psnr_hvs}` feature score is
byte-identical between the scalar path and the AVX2 path on all
three Netflix golden pairs**.

Vectorization strategy:

- **DCT butterfly**: load 8×8 block into 8× `__m256i` (one register
  per row, 8 int32 per register); apply the 30-butterfly
  `od_bin_fdct8_simd` once across all 8 rows in parallel; transpose
  the 8×8 int32 matrix; apply `od_bin_fdct8_simd` again across the
  columns; transpose back. Scheme: **butterfly → transpose →
  butterfly → transpose**.
- **Fixed-point arithmetic**: every `(x * k + round) >> shift`
  pattern uses `_mm256_madd_epi16`/`_mm256_mullo_epi32` +
  `_mm256_add_epi32` + `_mm256_srai_epi32` to reproduce the scalar
  arithmetic bit-for-bit. `OD_UNBIASED_RSHIFT32` is implemented via
  `_mm256_srli_epi32(_mm256_sub_epi32(_mm256_set1_epi32(0), x), 32-b)`
  `_mm256_add_epi32` (equivalent to scalar
  `(((uint32_t)x >> (32-b)) + x) >> b`).
- **Float accumulators kept scalar**: means, variances, mask, and
  per-coefficient error accumulation stay in the outer `for each
  8×8 block` loop in their original order. These are trivially
  bit-exact to scalar by construction (ADR-0139 precedent: avoid
  horizontal-reduce vectorization on float accumulators unless
  per-lane scalar tail is guaranteed).
- **FMA off**: `#pragma STDC FP_CONTRACT OFF` at the TU header;
  gcc -O3 was verified not to emit FMAs under the block-level
  scalar accumulators.

Runtime dispatch: new `PsnrHvsState` struct in `psnr_hvs.c` with
a `double (*calc_psnrhvs)(...)` function pointer; `init()` selects
`calc_psnrhvs_avx2` when `flags & VMAF_X86_CPU_FLAG_AVX2`,
otherwise the scalar.

NOLINT accounting (all with inline ADR-0141 citations):

- `od_bin_fdct8_simd` exceeds `readability-function-size` — the
  30-butterfly network must stay together for line-by-line diff
  against scalar `od_bin_fdct8`.
- Two `sqrt` calls in `compute_masks` trip
  `performance-type-promotion-in-math-fn` — `sqrt(double)`
  matches scalar's `float→double` promotion; switching to `sqrtf`
  would break the bit-exact contract.
- Scoped `NOLINTBEGIN/END` around upstream Xiph `od_bin_fdct8`,
  `od_bin_fdct8x8`, `calc_psnrhvs` in `psnr_hvs.c` — covers
  function-size, brace-style, pointer-offset widening, and
  sqrt promotion; these are the reference the AVX2 TU diffs
  against line-for-line and are kept verbatim for rebase parity
  with upstream Xiph.
- `vmaf_fex_psnr_hvs` needs `misc-use-internal-linkage` — extractor
  registry iterates over `vmaf_fex_*` externs.
- Test-side `ref_od_bin_fdct8` trips `readability-function-size`
  for the same load-bearing scalar-reference reason.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Bit-exact DCT via 8-wide int32 AVX2 (this ADR)** | Preserves Netflix golden numerically; no tolerance arguments; matches ADR-0138/0139 bar | 30-butterfly × 2 passes + 2 transposes is complex; careful `OD_UNBIASED_RSHIFT32` emulation needed | **Chosen** — matches fork's established bit-exactness discipline |
| **Float DCT with tolerance** | Simpler intrinsics; faster on some microarchitectures | Requires separate Netflix-golden tolerance ADR; opens drift risk on future SIMD variants; breaks the "scalar = SIMD" contract | Rejected — fork rule is "SIMD must match scalar" absent explicit ADR loosening |
| **Vectorize across blocks (not within a block)** | Easier data layout | psnr_hvs strides blocks by 7 with overlap; per-block dependency on accumulators blocks parallelization cleanly | Rejected — the butterfly-per-row-lane scheme gives ~8× and the outer loop isn't the bottleneck |
| **AVX2 + NEON in one PR** | Single review for both ISAs | Larger review surface; a bug in one ISA blocks the other; T3-4 precedent split them | Rejected via popup — AVX2 first, NEON follow-up |
| **Skip psnr_hvs SIMD entirely** | Zero work | Backlog says biggest SIMD perf win remaining; scalar is a real hot spot at 1080p+ | Rejected — demonstrated 3.58× DCT speedup worth the port cost |

## Consequences

- **Positive**:
  - **3.58× DCT speedup** on the hot inner kernel (isolated
    microbenchmark: 11.0 → 39.3 Mblocks/s at `-O3 -mavx2 -mfma`).
    Real-world frame-level speedup scales with resolution — at
    1080p × 3 planes the DCT is the dominant cost.
  - Byte-identical numerical output to scalar on all three Netflix
    golden CPU pairs (verified by diffing per-frame
    `psnr_hvs_{y,cb,cr,psnr_hvs}` XML fields between
    `VMAF_CPU_MASK=0` and default). Netflix golden `assertAlmostEqual`
    contract preserved by construction.
  - Fork's ISA-parity matrix grows: scalar + AVX2 + (NEON on
    follow-up). No AVX-512 path on fork since AVX2 covers the
    common case; future work only if a specific consumer needs it.
  - New unit test `test_psnr_hvs_avx2.c` pins the bit-exactness
    contract via DCT-level scalar-vs-SIMD diffs on 5 reproducible
    seeds; runs in `meson test -C build` (36/36 total now).
- **Negative**:
  - DCT butterfly SIMD code is not easy-read — reviewer needs the
    scalar reference open side-by-side. Mitigated by keeping the
    scalar TU's comment structure verbatim + scoped NOLINT so the
    scalar file stays unchanged except for `#include` + init
    dispatch plumbing.
  - New x86-only TU in the build; meson `x86_avx2_sources` gains
    one entry; the build flag `-mavx2` is already pinned for that
    list.
- **Neutral / follow-ups**:
  - **NEON follow-up PR** (backlog T3-5-neon — to be filed)
    mirrors this ADR's bit-exactness invariant. Reuses the same
    unit test harness (swap the AVX2 call for NEON).
  - AVX-512 path intentionally out of scope; AVX2 covers the
    x86_64 baseline and adding 512 means re-verifying bit-exactness
    against a different reduction tree.
  - The Netflix-golden Python tests (`quality_runner_test.py`,
    `vmafexec_test.py` etc.) don't currently pin a `psnr_hvs`
    assertion — this port doesn't introduce a new test; it
    preserves whatever values are checked today.

## Verification

- `meson test -C build` → **36/36 pass** (was 35; + new
  `test_psnr_hvs_avx2`).
- `test_psnr_hvs_avx2`: 5 subtests — 3 random 12-bit 8×8 seeds +
  delta-input + constant-input — DCT output byte-identical between
  scalar `od_bin_fdct8x8` and AVX2 `od_bin_fdct8x8_avx2`.
- **Netflix golden pair bit-exactness** (scalar vs AVX2 via
  `--cpumask $((~0x8))` vs default):

  ```text
  BIT-EXACT: src01_hrc00_576x324.yuv vs src01_hrc01_576x324.yuv (576×324, bpc=8)
  BIT-EXACT: checkerboard_1920_1080_10_3_0_0.yuv vs ..._1_0.yuv  (1920×1080, bpc=10)
  BIT-EXACT: checkerboard_1920_1080_10_3_0_0.yuv vs ..._10_0.yuv (1920×1080, bpc=10)
  ```

  Per-frame `psnr_hvs_y/cb/cr/psnr_hvs` values match byte-for-byte.
- `clang-tidy -p build --quiet` on all 4 touched files → **exit 0**.
- **Microbenchmark** (isolated DCT, 1M 8×8 blocks,
  `gcc -O3 -mavx2 -mfma`):

  | Variant | Mblocks/s | Speedup |
  | --- | ---: | ---: |
  | scalar | 11.0 | 1.00× |
  | AVX2 | 39.3 | **3.58×** |

- Small-fixture CLI wallclock speedup is ~2% (framework overhead
  dominates at 576×324 × 48 frames) — real gains scale with
  resolution.

## References

- Xiph/Daala DCT source:
  [`core/src/feature/third_party/xiph/psnr_hvs.c`](../../core/src/feature/third_party/xiph/psnr_hvs.c)
  (BSD-licensed, `Copyright 2001-2012 Xiph.Org`).
- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) — AVX2
  convolve bit-exact via double accumulators.
- [ADR-0139](0139-ssim-simd-bitexact-double.md) — SSIM per-lane
  scalar-float reduction for bit-exactness.
- [ADR-0140](0140-simd-dx-framework.md) — fork-internal SIMD DX
  macros (used in this port where they apply).
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule (scope of the NOLINTs above).
- [ADR-0145](0145-motion-v2-neon-bitexact.md) — NEON-after-AVX2
  port precedent (motion_v2 NEON landed separately after AVX2
  existed; this PR mirrors the split for psnr_hvs).
- [rebase-notes 0052](../rebase-notes.md) — upstream-sync
  invariants for this decision.
- Backlog: `.workingdir2/BACKLOG.md` T3-5.
- User direction 2026-04-24 popup: "T3-5 scope: AVX2 first, NEON
  follow-up PR (Recommended)".
