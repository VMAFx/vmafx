<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1455: `float_psnr_cuda` adds its squared differences as integers, and is bit-identical to the CPU

- **Status**: Accepted (Superseded-in-part 2026-10-06 by [ADR-1499](1499-float-psnr-twins-cpu-row-order.md) for the bound past 2^53 units for float_psnr_cuda)
- **Date**: 2026-10-02
- **Deciders**: lusoris
- **Tags**: `cuda`, `gpu-parity`, `numerics`, `psnr`, `testing`, `ci`, `rc3`, `fork-local`

## Context

`float_psnr.c` converts both luma planes to `float` (dividing a sample by
`scaler` = 2^(bpc - 8)), squares each difference in `float` and adds the
squares in `double`, row by row. Every term is a `float` and a multiple of
1 / `scaler`^2, so the sum is exact while it is below 2^53 of those units,
and the CPU's noise is the exact sum of its terms.

`float_psnr_cuda` formed the same terms and added them in fp32: per warp and
per 16x16 block, with the blocks added in `double` on the host. A block of
256 terms is exact in fp32 only while its sum needs at most 24 bits in that
unit. At 8 bits that always holds (256 squares below 2^16). At 10, 12 and 16
bits it holds while the block's rms difference stays below 256 code values,
which natural content at moderate distortion does.
[ADR-1440](1440-hip-float-psnr-exact-block-sums.md) found and fixed this in
the HIP twin and [ADR-1450](1450-sycl-float-psnr-exact-block-sums.md) in the
SYCL twin; both read the CUDA twin's source and left it to its lane.

Measured on an RTX 4090 at `--precision max` against `--backend cpu` on
master `2096bd1bb`, frames identical to the CPU's and the largest difference:

| Fixture | Frames | Identical | Max abs diff (dB) |
|---|---|---|---|
| Netflix 576x324 at 8, 10, 12 and 16 bit and as 10-bit 4:2:2, both 1080p checkerboards, Sparks 10 bit, BBB 3840x2160, noise at 8 bit | 167 | 167 | 0 |
| Full-range noise 576x324 at 10, 12 and 16 bit | 9 | 0 | 2.5e-8 |
| Bright 16 bit, 1920x1080 (samples 56000 to 64000) | 2 | 0 | 7.6e-8 |
| BBB 1920x1080 as 16 bit (each sample times 257) | 40 | 1 | 4.0e-8 |
| BBB 3840x2160 as 16 bit | 32 | 0 | 4.0e-8 |
| Noise at 40x40, 56x56 and 64x64, 8 and 10 bit | 18 | 10 | 1.2e-7 |

The repository's 10-, 12- and 16-bit Netflix fixtures are the 8-bit clip
shifted left, so nothing in the parity gate's fixtures showed it.

## Decision

We will add the squares as integers. `fpsnr_square()` in
`float_psnr/float_psnr_score.cu` squares the sample difference with
`__fmul_rn()`, which is the CPU's term times `scaler`^2 (the same
significand: up to 12 bits the square is exact, at 16 bits both round it to
24 bits), and converts the product, an integer below 2^32, to
`unsigned long long`. The warps and the blocks reduce 64-bit integers (a
block's 256 terms are below 2^40), the host reads one `uint64` per block,
adds them in `uint64`, converts the exact total to `double` and divides by
`scaler`^2 and the pixel count. Every step is exact below 2^53 units, so the
twin's noise is the CPU's. One templated kernel body serves both sample
types; the 16bpc kernel no longer takes the bit depth.
`scripts/ci/exact_twins.d/float_psnr.cuda` declares the twin exact.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `uint64` sums per warp and block (this ADR) | Exact at every bit depth with one kernel body; CUDA shuffles 64-bit values, as `psnr_score.cu` and `moment_score.cu` do; no measurable cost | The readback doubles (8 bytes per block, 259 KB at 3840x2160) | Chosen |
| `uint32` sums up to 12 bits, two `uint32` sums (low and high 16 bits of each square) at 16, as the HIP twin | 32-bit shuffles | Two code paths; HIP needed it because its wave shuffle is 32 bits wide | Not needed here |
| fp64 block sums | Three type changes | Exact as well, but the order of a double sum matters past 2^53, where an integer sum has none | The integer is exact by type |
| One `uint64` atomic per frame, as `psnr_cuda` | One value to read back | The block layout and the readback already exist | No gain over the block sums |
| Keep fp32 sums and a tolerance | No change | Not the CPU's score on high-bit-depth input with large differences, and nothing in the gate's fixtures shows it | The defect is invisible on natural content, which is the reason to remove it |

## Consequences

- **Positive**: measured on an RTX 4090 at `--precision max`, the score of
  every frame equals `--backend cpu`: 268 of 268 on the fixtures above (178
  before); the same with `uncapped=true`.
- **Positive**: no measurable cost; the numbers are in
  [the PSNR page](../metrics/psnr.md#float_psnr).
- **Negative**: at 16 bits the equality has a bound that comes from the CPU,
  not from the twin. The CPU's running sum is exact below 2^53 units of
  2^-16, a mean squared error of `2^37 / (width * height)` on the 8-bit scale:
  16570 at 3840x2160 (a PSNR below 6 dB), 66000 at 1920x1080 (above the
  largest possible error). Beyond it the CPU rounds each further add of a
  row's sum, and the twin, which rounds the exact sum once, is within
  `10 / ln(10) * (rows + 1) * 2^-53` dB of it plus a few units in the last
  place (7e-13 dB at 1440 rows). `test_cuda_float_psnr_parity` asserts that
  bound on a 2560x1440 frame past 2^53; the two scores were equal there.
- **Negative**: stored `float_psnr_cuda` scores of high-bit-depth input with
  large differences change by up to 1.2e-7 dB.
- **Neutral / follow-ups**:
  - `float_psnr_metal` reduces in fp32 the same way by its source
    (`T-METAL-FLOAT-PSNR-FP32-BLOCK-SUMS-2026-10-02`).
  - `test_hip_float_psnr_parity.c` carries its own copy of the fixtures that
    `float_psnr_twin_parity.h` holds for the SYCL and CUDA tests; moving the
    HIP test onto the header is left to the RC5 dedupe pass.
  - Guards: `test_cuda_float_psnr_parity` and its `_large` variant (`==` on
    two frames of full-range noise at 8, 10, 12 and 16 bits, with `uncapped`,
    on a bright 16-bit 1920x1080 pair and on identical frames; the bound past
    2^53; the 10-bit case fails on the fp32 twin) and
    `test_cuda_float_psnr_exact_contract.py` (seven planted regressions, no
    device).

## References

- `req` (coordinator brief for the CUDA lane, 2026-10-02): "`float_psnr_cuda` reduces in fp32 block sums (HIP #1779 / ADR-1440, SYCL #1794 / ADR-1450)".
- `req` (same brief): "One PR per twin that is not identical."
- [ADR-1440](1440-hip-float-psnr-exact-block-sums.md),
  [ADR-1450](1450-sycl-float-psnr-exact-block-sums.md),
  [ADR-1403](1403-cuda-strict-fp-every-kernel.md),
  [ADR-1215](1215-cuda-psnr-16bpc-plane-argument.md),
  [ADR-1421](1421-rc3-rc8-candidate-map.md),
  [ADR-1428](1428-exact-twins-fragments.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md).
- `docs/state.md`: `T-CUDA-FLOAT-PSNR-FP32-BLOCK-SUMS-2026-10-02` (opened and
  closed by this decision), `T-METAL-FLOAT-PSNR-FP32-BLOCK-SUMS-2026-10-02`.
