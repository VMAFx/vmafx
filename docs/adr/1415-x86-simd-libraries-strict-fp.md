<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1415: every x86 SIMD library is built without FP contraction

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `build`, `simd`, `numerics`, `icx`, `float-ms-ssim`, `testing`, `rc3`, `fork-local`

## Context

The x86 SIMD kernels are bit-exact twins of scalar references
([ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md),
[ADR-0139](0139-ssim-simd-bitexact-double.md)). The references live in
baseline libraries, where no fused multiply-add instruction exists, so an
`a * b + c` there always rounds twice. The SIMD files are built with `-mfma`
or `-mavx512f`, and most of them finish the last `n % lanes` elements of a
row in plain C. A compiler that contracts those expressions rounds the tail
once.

`core/src/meson.build` handled this file by file: nine files had their own
"carve-out" library with `vmaf_strict_fp_args` (contraction off), added one
at a time as a test or a lane failed, and the two general libraries
(`x86_avx2`, `x86_avx512`) took `vmaf_fp_model_args` only. Under icx that is
`-fp-model=precise`, which implies `-ffp-contract=on`. Every SYCL build is an
icx build.

Measured on `ryzen-4090-arc` (9950X3D, AVX-512, icx 2026.0 against GCC,
2026-10-01):

- `ssim_avx512.c` had no carve-out (its AVX2 sibling has one). The icx object
  held 24 FMA instructions, 13 of them scalar: the tails of
  `ssim_variance_avx512` (`sigma -= mu * mu`) and `ssim_accumulate_avx512`
  (`rm * rm + cm * cm + C1`). The CPU `float_ms_ssim` of an icx build
  differed from a GCC build and from its own scalar path by one fp32 unit in
  a per-scale mean, 7.7e-9 to 1.4e-8 in the score, on 4 of 104 frames (the
  Netflix 576x324 pair, both 1080p checkerboard pairs, BBB 3840x2160). With
  AVX-512 masked off the builds agreed.
- `adm_avx2.c` and `adm_avx512.c` held 42 FMA instructions each under icx
  and none under GCC, `vif_avx2.c` and `vif_statistic_avx2.c` 2 and none,
  `speed_avx2.c` and `speed_avx512.c` 43 and 47 against 3. No score
  difference was measured for these on the fixtures above.

This surfaced while making `float_ms_ssim_sycl` exact: the twin matched a GCC
CPU build on all 104 frames and the CPU extractor of its own binary on 100.

## Decision

We will build every x86 SIMD library with `vmaf_strict_fp_args`: the two
general libraries get the flag list the carve-outs already have. A kernel
that wants a fused multiply-add writes the intrinsic (`_mm256_fmadd_ps`,
`_mm512_fmadd_ps`) or `vmaf_fmaf_exact()`; no file relies on the compiler to
contract.

`test_strict_fp_compiler_args.py` lists `x86_avx2_static_lib` and
`x86_avx512_static_lib` as strict targets. The existing carve-out libraries
stay as they are; they carry the same flags.

The test `test_integer_adm_simd` compiles the scalar ADM kernels into its own
translation unit and was built without any FP flag, so under icx's default
fast model its reference differed from the kernels it checks
(`adm_cm_avx2 12x10 dense`: `0x1.06965p+3` against `0x1.06964ep+3`); it now
takes `_simd_strict_fp_args` like the other tests that carry a scalar
reference.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Strict flags on both general libraries (this ADR) | One policy; a new SIMD file is covered when it is added; GCC's objects do not change | The nine carve-out libraries become redundant | Chosen |
| A tenth carve-out for `ssim_avx512.c` only | Smallest diff | Leaves `adm`, `vif` and `speed` contracted under icx, and the next file with a plain-C tail repeats the defect; this is how the AVX-512 file was missed when the AVX2 one was fixed | The per-file list is the cause |
| Fold the carve-outs back into the general libraries | Less build code | Churn in a file every backend PR touches, no numerical effect | Left for a cleanup |
| Rewrite every scalar tail with intrinsics | Independent of flags | Many files; a new tail in plain C would still need the flag | The flag covers what the rewrite would |
| Leave icx builds as they are and compare twins against GCC builds only | No change | The parity gate runs one binary: a SYCL twin that is exact against the reference would fail against its own build's CPU extractor | The gate needs the icx CPU path to be the reference |

## Consequences

- **Positive**: the CPU extractors of an icx build give the GCC build's
  values on the Netflix 576x324 pair, both 1080p checkerboard pairs and BBB
  3840x2160 (20 frames) for 15 of 19 features, `float_ms_ssim` now among
  them; the other four differ through the math library, not through SIMD
  (below). Under icx only explicit fused multiply-adds remain in the two
  libraries (`common/convolution_avx512.c` 11, `ms_ssim_decimate_avx512.c`
  54, `speed_avx2.c` and `speed_avx512.c` 21 each). The new
  `test_ssim_x86_simd` compares the AVX2 and AVX-512 `ssim_precompute`,
  `ssim_variance` and `ssim_accumulate` with transcriptions of the scalar
  reference bit for bit at 24 element counts; on the unfixed icx build it
  fails at `ssim_variance_avx512 n=1`.
- **Negative**: none measured. GCC's 28 objects of the two libraries have the
  same disassembly with and without the flag, so GCC builds, the Netflix
  golden gate among them, compute what they did. The icx objects lose
  contractions the compiler chose, which were never part of a kernel's
  contract; their speed was not measured.
- **Neutral / follow-ups**: an icx build still differs from a GCC build where
  an extractor calls the math library on the host, because icx links Intel's
  `libimf` ahead of glibc's `libm`: `psnr` and `psnr_hvs` (7.1e-15), `ciede`
  (5.7e-12) and `speed_chroma` (1.2e-6) on the same fixtures, scalar and SIMD
  alike (`T-ICX-LIBIMF-HOST-MATH-2026-10-01`). The carve-out libraries can be
  folded into the general ones in a cleanup.

## References

- `req` (maintainer brief for the SYCL exactness lane, 2026-10-01): "results before speed; a twin reproduces the CPU bit for bit, tuning comes afterwards" and "Bugs you find on the way get fixed (own small PR when out of scope), not just recorded."
- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md),
  [ADR-0139](0139-ssim-simd-bitexact-double.md) (the bit-exact SIMD
  contract), [ADR-1317](1317-golden-gate-build-isolation.md) (the golden gate
  runs a GCC or clang build),
  [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md),
  [ADR-1403](1403-cuda-strict-fp-every-kernel.md) and
  [ADR-1407](1407-hip-strict-fp-every-kernel.md) (the same policy for the GPU
  kernels).
- `docs/state.md`: `T-ICX-SSIM-AVX512-FP-CONTRACT-2026-10-01`,
  `T-ICX-X86-SIMD-GENERAL-LIBS-CONTRACT-2026-10-01`,
  `T-ICX-INTEGER-ADM-SIMD-TEST-FP-MODEL-2026-10-01` (closed by this
  decision); `T-ICX-LIBIMF-HOST-MATH-2026-10-01` (opened).
