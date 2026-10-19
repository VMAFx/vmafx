<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1447: `float_moment_hip` adds the float squares the CPU adds, and is bit-identical while the CPU's own sum is exact

- **Status**: Accepted (Superseded-in-part 2026-10-06 by [ADR-1497](1497-float-moment-twins-cpu-sum-past-2-53.md) for the deferral of a bit-identical sum past 2^53 units for float_moment_hip)
- **Date**: 2026-10-02
- **Deciders**: lusoris
- **Tags**: `hip`, `gpu-parity`, `numerics`, `float-moment`, `testing`, `ci`, `rc3`, `fork-local`

## Context

`float_moment` reports the mean and the mean square of the reference and the
distorted luma plane. `moment.c` adds the samples, and their squares, into one
`double` per output and divides by the pixel count. `picture_copy()` has
divided each sample by 1, 4, 16 or 256 before, and
`compute_2nd_moment()` forms each square in `float`
(`const float term = pic_ * pic_`).

`float_moment_hip` accumulates four `uint64` sums on the device and divides
them on the host. Its second sums were the exact integer squares of the raw
samples. Up to 12 bits a square has at most 24 significant bits, the float
square is the integer square, and the twin returned the CPU's bits. At 16 bits
the float square is the integer square rounded to 24 bits and the twin did
not (`T-HIP-FLOAT-MOMENT-16BIT-SQUARES-2026-10-01`). Measured on a gfx1036
(ROCm 7.2.4) at `--precision max` against `--backend cpu`, second moments
identical to the CPU's and their largest difference:

| Fixture | Frames | Identical | Max abs diff |
|---|---|---|---|
| Typical content: Netflix 576x324 at 8 and 10 bit, both 1080p checkerboards, Sparks 10 bit, BBB 3840x2160 | 110 | 110 | 0 |
| Netflix 576x324 at 12 and 16 bit, and as 10-bit 4:2:2; noise at 8, 10 and 12 bit | 63 | 63 | 0 |
| Full-range noise 576x324, 16 bit | 3 | 0 | 2.8e-5 |
| Bright 16 bit, 1920x1080 (samples 56000 to 64000) | 2 | 0 | 1.0e-4 |
| BBB 1920x1080 as 16 bit (each sample times 257) | 40 | 0 | 7.5e-5 |
| BBB 3840x2160 as 16 bit | 32 | 0 | 3.9e-5 |

The repository's 16-bit Netflix fixture is 8-bit content shifted left, whose
squares have few significant bits, which is why the parity tests never saw
it. The first moments were identical everywhere.

## Decision

We will make the 16-bit kernel add the CPU's term.
`moment_float_square()` in `moment_score.hip` converts the raw sample to
`float`, multiplies it by itself in `float` (one product, rounded to nearest
even, as on the CPU) and converts the result, an integer below 2^32, to
`uint64`. Dividing a sample by a power of two before squaring does not change
which bits the rounding drops, so this integer is the CPU's term in units of
1 / scaler^2. The reduction, the readback and the host's two divisions stay
as they are.

Every term is a multiple of the unit. The CPU's running `double` sum is
therefore exact, and equal to the device's integer sum, while it is below
2^53 units. A term is below 2^32 units, so that holds for every frame of up
to 2^21 = 2 097 152 pixels at any content (1920x1080 has 2 073 600) and for
every frame at 8, 10 and 12 bits. `scripts/ci/exact_twins.d/float_moment.hip`
declares the twin exact for that range.

On a 16-bit frame of more than 2^21 pixels whose second moment times the
pixel count reaches 2^37 the CPU's sum passes 2^53 units. From there the CPU
rounds every add of a term that is not a multiple of the sum's last place,
and the twin, which holds the exact sum and rounds once, can differ from it.
The distance is at most

    (pixels - 2^21 + 1) / pixels * 2^(e - 69) + 2^-37

with `e` the binade of the sum in units (53 or more): at least 2^21 adds are
exact before the sum can reach 2^53, each later add rounds by at most half a
unit in the last place (2^(e-53) units), the twin's one rounding is within
the same, a unit is 2^-16 of a moment divided by the pixel count, and the two
final divisions add at most 2^-37. That is 2.3e-5 at 3840x2160 with every
sample near the peak (`e` = 54). `test_hip_float_moment_parity` asserts
equality in the exact range and this bound past it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add the float square as an integer (this ADR) | The CPU's term; the sum stays an exact integer; no change to the reduction or the host; no measurable cost | Not the CPU's bits past 2^53 | Chosen |
| Convert the integer square to `float` | The same value when the device converts an integer to `float` by rounding to nearest even | That conversion would have to be measured; the fp32 product is the CPU's own operation and is already covered by the strict FP build | The product needs no further assumption |
| Add the squares in `double` on the device | No integer detour | A parallel `double` sum rounds in another order past 2^53 and gains nothing below | The integer sum is exact and order-free |
| Reproduce the CPU's sequence of roundings past 2^53 with `ordered_sum.h` | Bit-identical on every frame | Four kernels and a walk per sum (the design of ADR-1433, 2.8 times the frame time on `ssimulacra2_hip`) for 16-bit frames above 2 097 152 pixels with a mean square above 2^37 / pixels | Deferred: `T-HIP-FLOAT-MOMENT-PAST-2-53-2026-10-02`. The bound is stated and tested |
| Keep the exact squares and a tolerance | No change | 1.0e-4 off at 16 bits, outside the 5e-5 gate tolerance | A defect |

## Consequences

- **Positive**: measured on a gfx1036 at `--precision max`, the four outputs
  of every frame equal `--backend cpu` on all 250 frames of the table above
  (712 outputs on the fourteen fixtures of the exactness sweep, 288 on the two
  16-bit BBB fixtures; before, the second moments of every 16-bit frame with
  full-range content differed). 17 of the 32 16-bit 3840x2160 frames are past
  2^53 units and identical too.
- **Positive**: no measurable cost. Steady state inside one process, medians
  of 11 interleaved pairs: 1.94 and 2.02 ms per 16-bit 1920x1080 frame before
  and after, 11.1 and 10.6 ms per 16-bit 3840x2160 frame (samples 8.0 to 16.8
  and 8.3 to 20.3 ms); the 8-bit kernel is unchanged (1.94 and 1.90 ms, 7.09
  and 7.10 ms).
- **Negative**: stored 16-bit `float_moment_hip` second moments change by up
  to 1.0e-4.
- **Neutral / follow-ups**:
  - Past 2^53 the twin is within the bound above and not bit-identical in
    general. Measured: 2.7e-7 on a 2560x1440 frame with a tenth of its
    samples below 4096 (bound 6.6e-6), 1.2e-7 on full-range 3840x2160 noise
    (bound 1.1e-5), 0 on a frame with every sample above 60000. The parity
    gate script compares an exact cell at 0 for every frame; it has no
    per-frame range, so a 16-bit fixture in that range would have to be added
    to it with this bound.
  - `float_moment_cuda` adds exact integer squares as well: on an RTX 4090 its
    second moments are 2.8e-5 and 1.0e-4 from the CPU on the same two 16-bit
    fixtures. `float_moment_sycl` and `float_moment_metal` do the same by
    their sources (not run here).
    `T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`.
  - Guards: `test_hip_float_moment_parity` and `_large` (noise at 8, 10, 12
    and 16 bits and a bright 16-bit 1920x1080 frame with `==`; the 16-bit
    cases fail on the old twin; a 2560x1440 frame past 2^53 against the
    bound) and `test_hip_float_moment_exact_contract.py` (four planted
    regressions, no device).

## References

- `req` (coordinator brief for the HIP lane, 2026-10-02): "`float_moment_hip` 16 bit (`T-HIP-FLOAT-MOMENT-16BIT-SQUARES-2026-10-01`): the kernel adds the CPU's terms; where the CPU's own double sum rounds above 2^53, derive the bound and gate that range with it, exact below."
- [ADR-1212](1212-gpu-moment-bit-depth-normalisation.md),
  [ADR-1433](1433-cuda-ssimulacra2-cpu-sum-order.md),
  [ADR-1437](1437-hip-exact-twins-declared.md),
  [ADR-1421](1421-rc3-rc8-candidate-map.md),
  [ADR-1428](1428-exact-twins-fragments.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md).
- `docs/state.md`: `T-HIP-FLOAT-MOMENT-16BIT-SQUARES-2026-10-01`,
  `T-HIP-FLOAT-MOMENT-PAST-2-53-2026-10-02`,
  `T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`.
