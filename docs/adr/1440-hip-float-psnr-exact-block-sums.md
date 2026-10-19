<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1440: `float_psnr_hip` adds its squared differences as integers and returns the CPU's score bit for bit

- **Status**: Accepted (Superseded-in-part 2026-10-06 by [ADR-1499](1499-float-psnr-twins-cpu-row-order.md) for the bound past 2^53 units for float_psnr_hip)
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `hip`, `psnr`, `gpu-parity`, `numerics`, `testing`, `ci`, `rc3`, `fork-local`

## Context

`float_psnr.c` converts both luma planes to `float` (dividing a sample by
`scaler` = 2^(bpc - 8)), squares each difference in `float` and adds the
squares in `double`, row by row. Every term is a `float` and the sum never
rounds (up to 12 bits it needs at most 49 bits at any frame size the library
accepts), so the CPU's noise is the exact sum of its terms.

`float_psnr_hip` formed the same terms and added them in fp32: per wave and
per 16x16 block, with the blocks added in `double` on the host. A block of
256 terms is exact in fp32 only while its sum needs at most 24 bits in the
unit of the smallest term, 1 / `scaler`^2. At 8 bits that always holds (256
squares below 2^16). At 10, 12 and 16 bits it holds while the block's rms
difference stays below 256 code values, which natural content at moderate
distortion does.

The RC3 sweep of the HIP twins on a gfx1036 (#1772) measured the twin
identical to the CPU on all 110 frames of typical content and on the
repository's 10-, 12- and 16-bit Netflix fixtures (which are the 8-bit clip
shifted left). On independent full-range noise it was 6.3e-9 dB off at 10
bits, 2.5e-8 at 12 and 1.8e-8 at 16, and 7.6e-8 on a bright 16-bit 1080p pair
(no frame identical); identical on 8-bit noise. A twin that is exact only
while the differences are small cannot be listed as exact
(ADR-1437's rule).

## Decision

We will add the squares as integers. The kernel squares the sample difference
in `float`, which is the CPU's term times `scaler`^2 (the same mantissa: up to
12 bits the square is exact, at 16 bits both round it to 24 bits), converts
the product to `uint32` and reduces `uint32` values per wave and per block.
Up to 12 bits one sum per block suffices (256 * 4095^2 < 2^32). At 16 bits
the low and the high 16 bits of each square are added separately. The host
reads two `uint32` per block, puts them together in `double`, adds the blocks
and divides by `scaler`^2. Every step is exact, so the twin's noise is the
CPU's. `float_psnr`: `hip` is declared an exact twin.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Integer block sums, two uint32 per block (this ADR) | Exact at every bit depth; 32-bit shuffles, so the time is unchanged within the noise (0.96 and 0.99 ms per 1080p frame, 3.97 and 3.98 ms per 4K frame) | The 16-bit kernel reduces twice; the read-back doubles (8 bytes per block) | Chosen |
| fp64 block sums through the wave shuffle | Three type changes | Measured: 0.91 to 1.93 ms per 1080p frame and 3.78 to 7.76 ms per 4K frame. HIP shuffles a double as two 32-bit halves | Twice the frame time |
| fp64 block sums through a shared-memory tree | No shuffle of doubles | Measured: 1.10 to 1.61 ms per 1080p frame, 5.12 to 6.68 ms per 4K frame (eight barrier rounds over 256 doubles) | 30 to 46 % more time |
| Keep fp32 sums and a tolerance | No change | Not the CPU's score on high-bit-depth input with large differences, and nothing in the gate's fixtures shows it | The defect is invisible on natural content, which is the reason to remove it |
| One uint64 atomic per frame, as `psnr_hip` | One value to read back | A 64-bit atomic per wave on one address; the block layout and the read-back already exist | No gain over the block sums |

## Consequences

- **Positive**: measured on a gfx1036 at `--precision max`, the score of
  every frame equals `--backend cpu`: 178 of 178 (167 before) on the Netflix
  576x324 pair at 8, 10, 12 and 16 bits and as 10-bit 4:2:2, both 1920x1080
  checkerboard pairs, Sparks 480x270 at 10 bits, 48 frames of BBB 3840x2160,
  full-range noise at four depths and a bright 16-bit 1080p pair; the same
  with `uncapped=true`.
- **Positive**: no measurable cost. Medians of 21 interleaved pairs of runs:
  0.96 and 0.99 ms per 1920x1080 frame, 3.97 and 3.98 ms per 3840x2160 frame,
  before and after.
- **Negative**: at 16 bits the equality has a bound that comes from the CPU,
  not from the twin. The CPU's running sum is exact below 2^53 units of
  2^-16, a mean squared error of 2^37 / (width * height) on the 8-bit scale:
  16570 at 3840x2160 (a PSNR below 6 dB), 66000 at 1920x1080 (above the
  largest possible error). Beyond it the CPU's sum rounds in sequence and no
  parallel sum reproduces it; the twin then returns the exact sum.
- **Neutral / follow-ups**: `float_psnr_cuda`, `_sycl` and `_metal` reduce in
  fp32 the same way (read from source, not run here); other lanes own them.
  Guards: `test_hip_float_psnr_parity` and its 960x540 registration (`==` on
  two frames of full-range noise at 8, 10, 12 and 16 bits; the 10-bit case
  fails on the fp32 twin) and `test_hip_float_psnr_exact_contract.py`.

## References

- `req` (maintainer brief for the second HIP lane, 2026-10-01): "Every HIP twin returns the CPU extractor's bits, or differs only by the math library with a derived bound."
- [ADR-1421](1421-rc3-rc8-candidate-map.md),
  [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md) (the exact cell),
  [ADR-1407](1407-hip-strict-fp-every-kernel.md),
  [ADR-0254](0254-hip-second-consumer-float-psnr.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md).
- `docs/state.md`: `T-HIP-FLOAT-PSNR-FP32-BLOCK-SUMS-2026-10-01` (opened and
  closed by this decision).
