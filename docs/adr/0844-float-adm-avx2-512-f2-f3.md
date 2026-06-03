# ADR-0844: float_adm AVX2/AVX-512 F2+F3 — double-precision and FP-contraction

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, bit-exactness, avx2, avx512, float_adm, build

## Context

The fork ships AVX2 and AVX-512 SIMD paths for `float_adm` at
[`core/src/feature/x86/float_adm_avx2.c`](../../core/src/feature/x86/float_adm_avx2.c)
and
[`core/src/feature/x86/float_adm_avx512.c`](../../core/src/feature/x86/float_adm_avx512.c).
Two defects were identified during an AVX2 audit of the ADM reduction
functions against the scalar reference in
[`core/src/feature/float_adm.c`](../../core/src/feature/float_adm.c):

**F2 — Reduction order mismatch (store-to-temp + scalar loop).**
`float_adm_csf_den_scale_avx2`, `float_adm_sum_cube_avx2`, and
their AVX-512 twins used the pattern:

```c
_Alignas(32) float tmp[8];
_mm256_store_ps(tmp, vcube);
for (int k = 0; k < 8; k++)
    row_accum += (double)tmp[k];
```

This stores 8 float lanes into a stack array, then accumulates them
one-by-one into a `double` running sum.  The cast `(double)tmp[k]`
widens each lane individually before addition, which is numerically
correct for each lane, but the accumulation order (lane 0 to lane 7,
left-to-right) differs from what a widening-then-horizontal-reduction
would produce.  The scalar reference in `float_adm.c` uses
`adm_sum_cube_s` / `adm_csf_den_scale_s`, which accumulate via a
C loop with double intermediates; the lane ordering in the SIMD path
did not match the scalar contract required by
[ADR-0139](0139-ssim-simd-bitexact-double.md).

**F3 — Auto-FMA inconsistency in the DWT2 vertical pass.**
Both `float_adm_avx2.c` and `float_adm_avx512.c` are compiled with
`-mfma` (inherited from the shared `x86_avx2_static_lib` build
target).  With `-mfma` enabled, the compiler is permitted to
auto-fuse adjacent `_mm256_mul_ps` + `_mm256_add_ps` pairs into FMA
instructions, producing a single-rounded result.  The scalar
reference in `float_adm.c` is compiled without `-mfma`, so the same
source pattern produces two-rounding `mul` + `add`.  This creates a
latent bit-level divergence between the SIMD and scalar paths on any
compiler that performs auto-contraction (GCC and Clang both do, with
`-ffp-contract=fast` or when `-mfma` implies it).  The precedent for
isolating this class of defect is already in tree: `ssimulacra2` uses
a per-TU static library with `-ffp-contract=off` for the same reason
([ADR-0594](0594-hip-ssimulacra2-blur-fp-contract-off.md) documents
the HIP analogue; the x86 ssimulacra2 carve-out is in
`core/src/meson.build` lines 694–705 as of master).

Both defects are in-scope under [ADR-0139](0139-ssim-simd-bitexact-double.md)
(SIMD float paths must match scalar bit-for-bit) and
[ADR-0108](0108-deep-dive-deliverables-rule.md) (architectural
decision requires this ADR).

## Decision

**F2 fix:** Replace the store-to-temp + scalar-accumulate loop with a
widening-to-double approach that is free of float-precision
intermediates and matches the scalar reference's double-precision
reduction contract:

- AVX2: use `_mm256_cvtps_pd` on the low 128-bit half
  (`_mm256_castps256_ps128`) and the high 128-bit half
  (`_mm256_extractf128_ps(v, 1)`) to produce two `__m256d` vectors of
  4 doubles each.  Sum each group with `hadd_pd4()` (a
  `_mm256_hadd_pd` + `_mm256_extractf128_pd` helper), add the two
  group sums.  No float-precision intermediate store occurs.
- AVX-512: use `hsum_ps_to_double()` which extracts four 4-float
  sub-vectors via `_mm512_extractf32x4_ps`, widens each to `__m256d`
  with `_mm256_cvtps_pd`, and reduces with `hadd_pd4`.  Four group
  sums are added left-to-right.

Both helper functions (`hadd_pd4` in each TU, `hsum_ps_to_double` in
the AVX-512 TU) are `static inline`, with `NOLINTNEXTLINE`
suppression citing ADR-0139 to document why outline-calling is
unsafe (register allocation changes rounding order on some ABIs).

**F3 fix:** Move `float_adm_avx2.c` and `float_adm_avx512.c` out of
the shared `x86_avx2_static_lib` / `x86_avx512_static_lib` and into
their own per-TU static libraries (`x86_float_adm_avx2_lib`,
`x86_float_adm_avx512_lib`) in `core/src/meson.build`.  Each
per-TU library adds `-ffp-contract=off` to its `c_args`, disabling
the compiler's auto-fusion of adjacent mul+add pairs into FMA
instructions for the entire TU.  The ISA flags (`-mavx`, `-mavx2`,
`-mfma` for AVX2; `-mavx512f` etc. for AVX-512) are preserved —
only auto-contraction is disabled.  This matches the pattern already
established for `ssimulacra2` in the same meson.build file.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep store-to-temp loop, document the difference | Zero code delta | Contradicts ADR-0139 bit-exactness contract; visible divergence at `--precision max` | Rejected — once identified, leaving a known bit-exact violation is a regression of the stated invariant |
| Per-lane scalar double reduction (ADR-0139 pattern) | Matches scalar C promotions exactly; proven in SSIM SIMD | Adds ~40 LoC of aligned temp buffers + scalar inner loops per TU; two levels of indirection | Rejected for F2 — `_mm256_cvtps_pd` widening is simpler and avoids the stack alloc entirely; for F3, the `-ffp-contract=off` carve-out is a cleaner build-system fix than restructuring the inner loop |
| Disable SIMD dispatch for `float_adm`, fall back to scalar | Guaranteed bit-exact, zero residual risk | Reverts a performance gain; upstream ports of `float_adm` SIMD additions would need to re-enable dispatch | Rejected — the defects are structural (precision model, FP contraction) and fixable without disabling the path |
| Ship AVX2 + AVX-512 + NEON all in one PR | Single review for all ISAs | NEON needs a separate arm64 CI leg; the fork's arm64 NEON SIMD coverage is already ahead of x86 (VIF, ADM); bundling triples review surface | Per project practice: AVX2 + AVX-512 only; NEON follow-up deferred |

## Consequences

- **Positive**: `float_adm_csf_den_scale_*` and `float_adm_sum_cube_*`
  are now numerically consistent between scalar, AVX2, and AVX-512
  reduction paths (no float intermediate store, double-precision
  throughout).  The DWT2 vertical pass in both ISA variants is
  protected against compiler auto-FMA at any optimization level.
- **Negative**: Two additional static libraries are compiled
  (`x86_float_adm_avx2_lib`, `x86_float_adm_avx512_lib`), adding a
  small build-time overhead.  The `-ffp-contract=off` flag disables
  auto-FMA for the entire `float_adm_avx*.c` TU, not just the DWT2
  pass — this is conservative but correct.
- **Neutral / follow-ups**:
  - `core/src/feature/AGENTS.md` rebase invariant: any future
    upstream port that changes `float_adm_csf_den_scale_s` or
    `float_adm_sum_cube_s` must propagate the change to the AVX2 and
    AVX-512 variants and verify double-precision reduction consistency.
  - The `hadd_pd4` helper is duplicated across AVX2 and AVX-512 TUs
    to avoid a cross-TU dependency (each TU has its own ISA flags).
    A shared header approach is not used because sharing would require
    a header compiled with neither `mavx2` nor `mavx512f` individually,
    which is unsupported by the current build model.
  - Reproducer (for PR description):

    ```bash
    vmaf --cpumask 255 --reference REF --distorted DIST \
         --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
         --feature float_adm --precision max -o scalar.xml
    vmaf --cpumask 4   --reference REF --distorted DIST \
         --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
         --feature float_adm --precision max -o avx2.xml
    diff <(grep float_adm scalar.xml) <(grep float_adm avx2.xml)   # empty
    ```

## References

- [ADR-0139](0139-ssim-simd-bitexact-double.md) — SIMD float paths must match scalar
  bit-for-bit; per-lane double reduction contract.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six deep-dive deliverables rule.
- [ADR-0594](0594-hip-ssimulacra2-blur-fp-contract-off.md) — ssimulacra2
  `-ffp-contract=off` carve-out precedent (HIP analogue).
- [Research-0110](../research/0110-adm-avx2-clz-ubsan-2026-05-15.md) — prior ADM
  AVX2 audit (UBSan / CLZ fix); this ADR covers the separate F2/F3 defects.
- Related PR: `fix/float-adm-avx2-f2-f3-pr116` (PR #142 on VMAFx/vmafx).
- Source: project decision — AVX2 + AVX-512 SIMD correctness audit of float_adm
  F2/F3 defects identified during fork ADR-0139 compliance sweep.
