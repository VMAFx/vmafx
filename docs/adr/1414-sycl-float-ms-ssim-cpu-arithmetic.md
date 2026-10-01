<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1414: `float_ms_ssim_sycl` computes the CPU's arithmetic and returns its per-scale means bit for bit

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `sycl`, `gpu-parity`, `numerics`, `float-ms-ssim`, `testing`, `ci`, `rc3`, `fork-local`

## Context

[ADR-1403](1403-cuda-strict-fp-every-kernel.md) found four differences
between `float_ms_ssim_cuda` and the CPU extractor, fixed them, and left the
SYCL, HIP and Metal twins, written from the same shaders, as
`T-GPU-FLOAT-MS-SSIM-CPU-ARITHMETIC-2026-10-01`:

1. the decimate accumulates `acc += sample * tap`, where `ms_ssim_decimate.c`
   fuses each tap (`vmaf_fmaf_exact()`);
2. the Gaussian window sums are fp32 running sums, where `iqa_convolve()`
   adds fp32 products in fp64 and rounds once per pass;
3. `l`, `c` and `s` are computed in fp32, where
   `ssim_accumulate_default_scalar()` divides fp64 numerators by fp32
   denominators (`l`, `c`) and takes an fp32 quotient (`s`);
4. the host combines unrounded per-scale means without `fabs()` on `l` and
   `c`, where `iqa_ssim()` returns floats and `ms_ssim.c` takes `fabs()` of
   all three.

Measured on an Arc A380 at `--precision max` against `--backend cpu`, the
SYCL twin matched the CPU on none of 104 frames: 6.9e-8 on the Netflix
576x324 pair, 1.06e-6 and 2.98e-6 on the two 1920x1080 checkerboard pairs,
1.23e-6 on BBB 3840x2160.

A SYCL kernel may not use the fp64 type
([ADR-0220](0220-sycl-fp64-fallback.md)), so the CUDA fix does not carry over
as written: that kernel computes `l` and `c` in `double` and sums them per
block in `double`. `float_ssim_sycl` already solved the same problem for the
single-scale metric (Research-2133): fp64 window sums and the fp64 `l` and
`c` quotients as exact fp32 pairs, and the frame sums in int64 fixed point,
which is exact and independent of the reduction order.

## Decision

We will make `float_ms_ssim_sycl` follow the CPU reference operation for
operation and type for type, using the pair arithmetic `float_ssim_sycl`
uses, from one shared header.

- **Decimate**: `decimate_pixel()` spells each tap `sycl::fma()`, row sums
  first and the nine row sums in the reference's tap order.
- **Window sums and `l` / `c` / `s`**: `core/src/feature/sycl/sycl_ssim_terms.h`
  holds `add_horizontal_tap()`, `add_vertical_tap()`, `round_moments()`,
  `ssim_terms()`, `term_fixed()` and `FixedSum`, moved out of
  `integer_ssim_sycl.cpp`. Both twins include it; neither keeps a copy.
- **Frame sums**: each pixel's `l`, `c` and `s` go to int64 units of 2^-52,
  `reduce_over_group` adds them exactly, the host adds the groups exactly
  (`FixedSum`).
- **Host**: each per-scale mean is `(float)(sum / pixels)`, as `iqa_ssim()`
  returns it, and `combine_ms_ssim()` takes `fabs()` of all three.
- **Gate**: `float_ms_ssim` and `float_ms_ssim_lcs` list `sycl` in
  `EXACT_TWINS` (`scripts/ci/cross_backend_calibration.py`).

What remains differs from the CPU only below the last bit of an fp32
per-scale mean: the CPU forms each `l` and `c` quotient in fp64 and adds them
into a running fp64 sum in raster order; the twin carries the quotients to
about 2^-46 and adds them exactly. The fp32 rounding of the mean absorbs
that unless the mean lies within about 2^-22 of a rounding boundary of its
last bit. The twin is therefore bit-identical in practice, not by
construction, as the CUDA twin is.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| The shared pair arithmetic of `float_ssim_sycl`, int64 frame sums (this ADR) | One implementation for both SSIM twins; proven exact on `float_ssim`; no fp64; no scratch memory | A pair division per `l` and `c` | Chosen |
| A second copy of the pair helpers in the MS-SSIM file | No change to `integer_ssim_sycl.cpp` | Two implementations of one arithmetic that must stay in step with the CPU | HISS-19 |
| fp64 on the device, as the CUDA kernel has | The CPU's own types | Rejects the whole module on fp64-less devices such as the Arc A-series (ADR-0220) | Not available |
| Software fp64 in integers, raster-order sum on the host | Exact by construction | Three 8-byte values per pixel and scale read back (200 MB per 3840x2160 frame at scale 0), or an emulated division and a sequential sum that no device parallelises | The cost is out of proportion to a residue below the last bit of the mean |
| Keep the fp32 arithmetic and the 5e-5 tolerance | No work | The twin differs from the CPU on every frame, in a direction that depends on the content | The contract is the CPU's value |

## Consequences

- **Positive**: on an Arc A380 every per-scale mean of every frame measured
  equals the CPU's: the Netflix 576x324 pair (48 frames), both 1920x1080
  checkerboard pairs (3 each) and BBB 3840x2160 (50 frames with
  `enable_lcs`, 16 outputs each), with `enable_chroma` (Cb and Cr of the
  checkerboards and 50 BBB frames), and the Netflix pair at 10, 12 and 16
  bits and as 4:2:2 10-bit. The score equals a GCC build's CPU score on 253
  of 254 frames (200 BBB frames in that run); the one that differs does so
  by 1.1e-16, because the twin's host combine calls the `pow()` of its own
  icx build (`T-ICX-LIBIMF-HOST-MATH-2026-10-01`). Against the CPU extractor
  of its own binary it is identical on every frame once that extractor is
  the scalar arithmetic (on an AVX-512 host an icx build needs #1706 for
  that).
- **Negative**: the pair arithmetic costs device time. Through the `vmaf`
  tool on the Arc A380: 31.4 ms per 3840x2160 frame before, 42.6 after
  (medians of 15 paired 100-frame runs, host load 10 to 20; the CPU
  extractor takes 117 ms on this host), and 0.84 to 1.20 ms per 576x324
  frame. `float_ssim_sycl`, which runs the same helpers, is unchanged (22.9
  and 23.0 ms at 3840x2160, `scale=1`). A cheaper tap accumulation (one
  two-sum per tap instead of a full pair addition) gives the same values and
  42.2 ms, so the taps are not where the time goes; it was not adopted. The
  kernels read 22 and 55 samples per pixel from device memory in the two
  passes, as before; staging them through local memory as `float_ssim_sycl`
  does in its horizontal pass is the tuning left for later.
- **Neutral / follow-ups**: `integer_ms_ssim_hip` and
  `float_ms_ssim_metal` keep the old arithmetic
  (`T-GPU-FLOAT-MS-SSIM-CPU-ARITHMETIC-2026-10-01` stays open for them).
  `float_ssim_sycl` produces the same values as before; its kernels did not
  change, only where their helpers live. Any stored `float_ms_ssim_sycl`
  output changes in its low digits (by at most the differences above). No
  kernel uses scratch memory (`test_sycl_kernel_scratch` on the A380, ratchet
  list unchanged). Guards: `test_sycl_ms_ssim_parity` and its 960x540 variant
  compare 18 outputs of 3 frames with `==` (53 and 54 of 54 differ on the old
  code, by up to 9.9e-8), and `test_sycl_kernel_source_contract.py` plants
  seven regressions.

## References

- `req` (maintainer brief for the SYCL exactness lane, 2026-10-01): "float_ms_ssim_sycl; apply the CPU arithmetic the CUDA kernel now uses" and "results before speed; a twin reproduces the CPU bit for bit, tuning comes afterwards".
- [ADR-1403](1403-cuda-strict-fp-every-kernel.md) (the four differences and
  the CUDA fix), [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md)
  (contraction off), [ADR-0220](0220-sycl-fp64-fallback.md),
  [ADR-1395](1395-sycl-kernels-no-scratch.md),
  [ADR-1363](1363-sycl-ssimulacra2-msssim-device-resident.md) (one wait per
  frame), [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md) (the exact gate
  cell), [ADR-0214](0214-gpu-parity-ci-gate.md).
- `core/src/feature/sycl/integer_ssim_sycl.cpp` (`float_ssim_sycl`: the pair
  arithmetic and the fixed-point sums this decision reuses; the source cites
  them as Research-2133).
- `docs/state.md`: `T-GPU-FLOAT-MS-SSIM-CPU-ARITHMETIC-2026-10-01` (SYCL part
  closed by this decision).
