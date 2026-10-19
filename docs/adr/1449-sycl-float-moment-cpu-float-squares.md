<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1449: `float_moment_sycl` adds the float squares the CPU adds, and is bit-identical while the CPU's own sum is exact

- **Status**: Accepted (Superseded-in-part 2026-10-06 by [ADR-1497](1497-float-moment-twins-cpu-sum-past-2-53.md) for the deferral of a bit-identical sum past 2^53 units for float_moment_sycl)
- **Date**: 2026-10-02
- **Deciders**: lusoris
- **Tags**: `sycl`, `gpu-parity`, `numerics`, `float-moment`, `testing`, `ci`, `rc3`, `fork-local`

## Context

`float_moment` reports the mean and the mean square of the reference and the
distorted luma plane. `moment.c` adds the samples, and their squares, into one
`double` per output and divides by the pixel count. `picture_copy()` has
divided each sample by 1, 4, 16 or 256 before, and `compute_2nd_moment()`
forms each square in `float` (`const float term = pic_ * pic_`).

`float_moment_sycl` accumulates four `int64` sums on the device and divides
them on the host. Its second sums were the exact integer squares of the raw
samples. Up to 12 bits a square has at most 24 significant bits, the float
square is the integer square, and the twin returned the CPU's bits. At 16
bits the float square is the integer square rounded to 24 bits and the twin
did not. [ADR-1447](1447-hip-float-moment-cpu-float-squares.md) found and
fixed the same defect in the HIP twin and left the SYCL twin, read from
source only, in `T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`.

Measured on an Arc A380 (xe driver) at `--precision max` against
`--backend cpu` of a GCC build, second moments identical to the CPU's and
their largest difference:

| Fixture | Frames | Identical | Max abs diff |
|---|---|---|---|
| Netflix 576x324 at 8 bit, both 1080p checkerboards, BBB 3840x2160 | 254 | 254 | 0 |
| Netflix 576x324 at 10, 12 and 16 bit and as 10-bit 4:2:2; noise at 8, 10 and 12 bit | 21 | 21 | 0 |
| Full-range noise 576x324, 16 bit | 3 | 0 | 2.7e-5 |
| Bright 16 bit, 1920x1080 (samples 56000 to 64000) | 2 | 0 | 1.0e-4 |
| BBB 3840x2160 as 16 bit (each sample times 257) | 8 | 0 | 3.9e-5 |

The repository's 16-bit Netflix fixture is 8-bit content shifted left, whose
squares have few significant bits, which is why the survey of the SYCL twins
on the gate's fixtures and the parity tests never saw it. The first moments
were identical everywhere.

## Decision

We will make the kernel add the CPU's term, as ADR-1447 did for HIP.
`moment_float_square()` in `integer_moment_sycl.cpp` converts the raw sample
to `float`, multiplies it by itself in `float` (one product, rounded to
nearest even, as on the CPU) and converts the result, an integer below 2^32,
to `int64`. Dividing a sample by a power of two before squaring does not
change which bits the rounding drops, so this integer is the CPU's term in
units of 1 / scaler^2. Up to 12 bits it is the integer square, so one kernel
serves every bit depth. The reduction, the read-back and the host's two
divisions stay as they are. The helper is always inlined: a call left in a
kernel takes scratch memory ([ADR-1395](1395-sycl-kernels-no-scratch.md)).

Every term is a multiple of the unit. The CPU's running `double` sum is
therefore exact, and equal to the device's integer sum, while it is below
2^53 units. A term is below 2^32 units, so that holds for every frame of up
to 2^21 = 2 097 152 pixels at any content (1920x1080 has 2 073 600) and for
every frame at 8, 10 and 12 bits. `scripts/ci/exact_twins.d/float_moment.sycl`
declares the twin exact for that range.

On a 16-bit frame of more than 2^21 pixels whose second moment times the
pixel count reaches 2^37 the CPU's sum passes 2^53 units. From there the CPU
rounds every add of a term that is not a multiple of the sum's last place,
and the twin, which holds the exact sum and rounds once, can differ from it
by at most the bound ADR-1447 derives,

    (pixels - 2^21 + 1) / pixels * 2^(e - 69) + 2^-37

with `e` the binade of the sum in units. `test_sycl_float_moment_parity`
asserts equality in the exact range and this bound past it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add the float square as an integer (this ADR) | The CPU's term; the sum stays an exact integer; no change to the reduction or the host; no measurable cost; the HIP twin's design | Not the CPU's bits past 2^53 | Chosen |
| A second kernel for 16 bits, the integer square kept below | The 8- to 12-bit path untouched | Two kernels for one value: the float square equals the integer square there | One kernel |
| Reproduce the CPU's sequence of roundings past 2^53 with `sycl_ordered_sum.h` ([ADR-1446](1446-sycl-ssimulacra2-cpu-bits.md)) | Bit-identical on every frame | A plan, increments and a walk per sum for 16-bit frames above 2 097 152 pixels with a mean square above 2^37 / pixels; `ssimulacra2_sycl` pays 2.3 times its frame time for that design | Deferred with the other twins: `T-HIP-FLOAT-MOMENT-PAST-2-53-2026-10-02`. The bound is stated and tested |
| Keep the exact squares and a tolerance | No change | 1.0e-4 off at 16 bits, outside the 5e-5 gate tolerance | A defect |

## Consequences

- **Positive**: measured on an Arc A380 at `--precision max`, the four
  outputs of every frame equal `--backend cpu` on all 288 frames of the table
  above (275 before; the second moments of every 16-bit frame with real low
  bits differed).
- **Positive**: no measurable cost. Medians of 7 runs of the `vmaf` tool,
  twin alone: 28.66 and 28.64 ms per 3840x2160 frame before and after, 0.63
  and 0.64 ms per 576x324 frame (a `float_psnr_sycl` control read 3.34 and
  3.27 ms).
- **Negative**: stored 16-bit `float_moment_sycl` second moments change by up
  to 1.0e-4.
- **Neutral / follow-ups**:
  - Past 2^53 the twin is within the bound above and not bit-identical in
    general: 2.7e-7 on the test's 2560x1440 frame (bound 6.6e-6). The parity
    gate script compares an exact cell at 0 for every frame; it has no
    per-frame range, so a 16-bit fixture in that range would have to be added
    to it with this bound. `T-HIP-FLOAT-MOMENT-PAST-2-53-2026-10-02` covers
    every twin.
  - The kernel adds to four global 64-bit counters once per pixel. That takes
    28.7 ms per 3840x2160 frame on the A380, where `float_psnr_sycl`, which
    reduces per work-group, takes 3.3 ms:
    `T-SYCL-FLOAT-MOMENT-PER-PIXEL-ATOMICS-2026-10-02`.
  - `float_moment_cuda` and `float_moment_metal` stay in
    `T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`.
  - Guards: `test_sycl_float_moment_parity` and `_large` (noise at 8, 10, 12
    and 16 bits and a bright 16-bit 1920x1080 frame with `==`; both 16-bit
    cases fail on the old twin; a 2560x1440 frame past 2^53 against the bound,
    which the old twin misses by a factor of 13) and
    `test_sycl_float_moment_exact_contract.py` (six planted regressions, no
    device). The cases live in `core/test/float_moment_twin_parity.h` for
    another backend's test to instantiate.

## References

- `req` (coordinator brief for the SYCL exactness lane, 2026-10-01): "User direction: results before speed; a twin reproduces the CPU bit for bit, tuning comes afterwards."
- `req` (same brief): "Then measure every other SYCL twin against the CPU at --precision max on the A380 (Netflix pair, both 1080p checkerboards, testdata/bbb 4K), list which are bit-identical and which are not with the max abs diff, and fix the non-identical ones one PR each in order of the largest difference, isolating the cause per term (types, rounding points, libm calls, reduction order)."
- [ADR-1447](1447-hip-float-moment-cpu-float-squares.md),
  [ADR-1212](1212-gpu-moment-bit-depth-normalisation.md),
  [ADR-1446](1446-sycl-ssimulacra2-cpu-bits.md),
  [ADR-1395](1395-sycl-kernels-no-scratch.md),
  [ADR-1437](1437-hip-exact-twins-declared.md),
  [ADR-1421](1421-rc3-rc8-candidate-map.md),
  [ADR-1428](1428-exact-twins-fragments.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md).
- `docs/state.md`: `T-SYCL-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`,
  `T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`,
  `T-HIP-FLOAT-MOMENT-PAST-2-53-2026-10-02`,
  `T-SYCL-FLOAT-MOMENT-PER-PIXEL-ATOMICS-2026-10-02`.
