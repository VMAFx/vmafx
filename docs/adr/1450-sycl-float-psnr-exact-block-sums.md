<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1450: `float_psnr_sycl` adds its squared differences as integers, and is bit-identical to the CPU

- **Status**: Accepted (Superseded-in-part 2026-10-06 by [ADR-1499](1499-float-psnr-twins-cpu-row-order.md) for the bound past 2^53 units for float_psnr_sycl)
- **Date**: 2026-10-02
- **Deciders**: lusoris
- **Tags**: `sycl`, `gpu-parity`, `numerics`, `psnr`, `testing`, `ci`, `rc3`, `fork-local`

## Context

`float_psnr.c` converts both luma planes to `float` (dividing a sample by
`scaler` = 2^(bpc - 8)), squares each difference in `float` and adds the
squares in `double`, row by row. Every term is a `float` and a multiple of
1 / `scaler`^2, and the sum does not round while it is below 2^53 of those
units, so the CPU's noise is the exact sum of its terms.

`float_psnr_sycl` formed the same terms and added them in fp32: per sub-group
and per 16x16 work-group, with the work-groups added in `double` on the host.
A group of 256 terms is exact in fp32 only while its sum needs at most 24
bits in the unit of the smallest term. At 8 bits that always holds (256
squares below 2^16). At 10, 12 and 16 bits it holds while the group's rms
difference stays below 256 code values, which natural content at moderate
distortion does. [ADR-1440](1440-hip-float-psnr-exact-block-sums.md) found
and fixed this in the HIP twin and named the SYCL twin from its source.

A sweep of every SYCL twin on high-bit-depth and full-range fixtures
([ADR-1449](1449-sycl-float-moment-cpu-float-squares.md)) measured it on an
Arc A380 (xe driver) at `--precision max` against `--backend cpu` of the same
build (`T-SYCL-FLOAT-PSNR-FP32-BLOCK-SUMS-2026-10-02`):

| Fixture | Frames | Identical | Max abs diff (dB) |
|---|---|---|---|
| Netflix 576x324 at 8, 10, 12 and 16 bit and as 10-bit 4:2:2, both 1080p checkerboards, BBB 3840x2160, noise at 8 bit | 269 | 269 | 0 |
| Full-range noise 576x324, 10 bit | 3 | 0 | 1.1e-8 |
| Full-range noise 576x324, 12 bit | 3 | 0 | 2.4e-8 |
| Full-range noise 576x324, 16 bit | 3 | 0 | 7.4e-9 |
| Bright 16 bit, 1920x1080 (samples 56000 to 64000) | 2 | 0 | 7.4e-8 |
| BBB 3840x2160 as 16 bit (each sample times 257) | 8 | 0 | 3.3e-8 |

The repository's high-bit-depth Netflix fixtures are the 8-bit clip shifted
left, which is why the earlier survey on the gate's fixtures saw nothing.

## Decision

We will add the squares as integers. The kernel squares the raw sample
difference in `float`, which is the CPU's term times `scaler`^2 (the same
significand: the CPU's samples are the raw ones divided by a power of two, so
up to 12 bits the square is exact and at 16 bits both round it to 24 bits),
and converts the product, an integer below 2^32, to `uint64`. The sub-group
reduction, the work-group total and the read-back are `uint64`. The host adds
the work-groups in `uint64`, converts the total to `double` and divides by
`scaler`^2 and by the pixel count. The device's sum is exact at every bit
depth, and the conversion is exact below 2^53, so the twin's noise is the
CPU's wherever the CPU's own sum is exact. `float_psnr`: `sycl` is declared
an exact twin.

`fpsnr_pixel_noise()` and `fpsnr_store_workgroup_sum()` are always inlined:
a call left in a kernel takes scratch memory
([ADR-1395](1395-sycl-kernels-no-scratch.md)).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `uint64` sums per sub-group and work-group (this ADR) | Exact at every bit depth with one kernel; 3 % more time at 3840x2160 (3.31 to 3.41 ms), none at 576x324 | The read-back doubles (8 bytes per work-group, 259 KB at 3840x2160) | Chosen |
| `uint32` sums up to 12 bits, two `uint32` sums (low and high 16 bits of each square) at 16, as the HIP twin | 32-bit arithmetic in the reduction | Two code paths; HIP needed it because its wave shuffle is 32 bits wide, SYCL reduces `uint64` directly | Not needed at 3 % |
| fp32 pair sums per work-group | No integer conversion | A pair holds 48 bits; a work-group needs 40, but the type would have to be reduced by hand | The integer is simpler and exact by type |
| Keep fp32 sums and a tolerance | No change | Not the CPU's score on high-bit-depth input with large differences, and nothing in the gate's fixtures shows it | The defect is invisible on natural content, which is the reason to remove it |

## Consequences

- **Positive**: measured on an Arc A380 at `--precision max`, the score of
  every frame equals `--backend cpu` of the same build: 288 of 288 on the
  fixtures above (269 before); the same with `uncapped=true`.
- **Negative**: 3 % more time at 3840x2160. Medians of 7 runs of the `vmaf`
  tool, twin alone, host load average 13: 3.31 ms before and 3.41 ms after
  per 3840x2160 frame, 0.12 and 0.12 ms per 576x324 frame (a `psnr_sycl`
  control read 2.93 and 2.94 ms).
- **Negative**: at 16 bits the equality has a bound that comes from the CPU,
  not from the twin. The CPU's running sum is exact below 2^53 units of
  2^-16, a mean squared error of `2^37 / (width * height)` on the 8-bit scale:
  16570 at 3840x2160 (a PSNR below 6 dB), 66000 at 1920x1080 (above the
  largest possible error). Beyond it the CPU rounds each further add of a
  row's sum, and the twin, which rounds the exact sum once, is within
  `10 / ln(10) * (rows + 1) * 2^-53` dB of it plus a few units in the last
  place (7e-13 dB at 1440 rows). `test_sycl_float_psnr_parity` asserts that
  bound on a 2560x1440 frame past 2^53; the two scores were equal there.
- **Negative**: stored `float_psnr_sycl` scores of high-bit-depth input with
  large differences change by up to 7.4e-8 dB.
- **Neutral / follow-ups**:
  - Against a GCC build of the CPU extractor 6 of 288 frames differ by at
    most 1.4e-14 dB. The noise is equal there; the score is
    `10 * log10(...)` on the host, and an icx build links Intel's `log10`
    where a GCC build links glibc's. The CPU extractor of an icx build
    differs from a GCC build on the same frames
    (`T-ICX-LIBIMF-HOST-MATH-2026-10-01`).
  - `float_psnr_cuda` and `float_psnr_metal` reduce in fp32 the same way by
    their sources; other lanes own them.
  - Guards: `test_sycl_float_psnr_parity` and its `_large` variant (`==` on
    two frames of full-range noise at 8, 10, 12 and 16 bits, with `uncapped`,
    on a bright 16-bit 1920x1080 pair and on identical frames; the bound past
    2^53; six of the nine score cases fail on the fp32 twin) and
    `test_sycl_float_psnr_exact_contract.py` (nine planted regressions, no
    device). The cases live in `core/test/float_psnr_twin_parity.h` for
    another backend's test to instantiate.

## References

- `req` (coordinator brief for the SYCL exactness lane, 2026-10-01): "User direction: results before speed; a twin reproduces the CPU bit for bit, tuning comes afterwards."
- `req` (same brief): "Then measure every other SYCL twin against the CPU at --precision max on the A380 (Netflix pair, both 1080p checkerboards, testdata/bbb 4K), list which are bit-identical and which are not with the max abs diff, and fix the non-identical ones one PR each in order of the largest difference, isolating the cause per term (types, rounding points, libm calls, reduction order)."
- [ADR-1440](1440-hip-float-psnr-exact-block-sums.md),
  [ADR-1449](1449-sycl-float-moment-cpu-float-squares.md),
  [ADR-1395](1395-sycl-kernels-no-scratch.md),
  [ADR-1193](1193-psnr-uncapped-option.md),
  [ADR-1421](1421-rc3-rc8-candidate-map.md),
  [ADR-1428](1428-exact-twins-fragments.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md).
- `docs/state.md`: `T-SYCL-FLOAT-PSNR-FP32-BLOCK-SUMS-2026-10-02`.
