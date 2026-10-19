<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1453: `float_moment_cuda` adds the float squares the CPU adds, and is bit-identical while the CPU's own sum is exact

- **Status**: Accepted (Superseded-in-part 2026-10-06 by [ADR-1497](1497-float-moment-twins-cpu-sum-past-2-53.md) for the deferral of a bit-identical sum past 2^53 units for float_moment_cuda)
- **Date**: 2026-10-02
- **Deciders**: lusoris
- **Tags**: `cuda`, `gpu-parity`, `numerics`, `float-moment`, `testing`, `ci`, `rc3`, `fork-local`

## Context

`float_moment` reports the mean and the mean square of the reference and the
distorted luma plane. `moment.c` adds the samples, and their squares, into one
`double` per output and divides by the pixel count. `picture_copy()` has
divided each sample by 1, 4, 16 or 256 before, and `compute_2nd_moment()`
forms each square in `float` (`const float term = pic_ * pic_`).

`float_moment_cuda` accumulates four `uint64` sums on the device and divides
them on the host. Its second sums were the exact integer squares of the raw
samples. Up to 12 bits a square has at most 24 significant bits, the float
square is the integer square, and the twin returned the CPU's bits. At 16
bits the float square is the integer square rounded to 24 bits and the twin
did not. [ADR-1447](1447-hip-float-moment-cpu-float-squares.md) found and
fixed the defect in the HIP twin, measured the CUDA twin on two fixtures and
left it in `T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`;
[ADR-1449](1449-sycl-float-moment-cpu-float-squares.md) fixed the SYCL twin.

Measured on an RTX 4090 at `--precision max` against `--backend cpu` on
master `2096bd1bb`, second moments identical to the CPU's and their largest
difference:

| Fixture | Frames | Identical | Max abs diff |
|---|---|---|---|
| Netflix 576x324 at 8 and 10 bit, both 1080p checkerboards, BBB 3840x2160 | 105 | 105 | 0 |
| Netflix 576x324 at 12 and 16 bit and as 10-bit 4:2:2, Sparks 10 bit, noise at 8, 10 and 12 bit | 68 | 68 | 0 |
| Full-range noise 576x324, 16 bit | 3 | 0 | 2.8e-5 |
| Bright 16 bit, 1920x1080 (samples 56000 to 64000) | 2 | 0 | 1.0e-4 |
| BBB 1920x1080 as 16 bit (each sample times 257) | 40 | 0 | 7.5e-5 |
| BBB 3840x2160 as 16 bit | 32 | 0 | 3.9e-5 |

The repository's 16-bit Netflix fixture is 8-bit content shifted left, whose
squares have few significant bits, which is why the parity tests never saw
it. The first moments were identical everywhere.

## Decision

We will make the 16-bit kernel add the CPU's term, as ADR-1447 and ADR-1449
did. `moment_float_square()` in `integer_moment/moment_score.cu` converts the
raw sample to `float`, multiplies it by itself with `__fmul_rn()` (one fp32
product, rounded to nearest even, as on the CPU) and converts the result, an
integer below 2^32, to `unsigned long long`. Dividing a sample by a power of
two before squaring does not change which bits the rounding drops, so this
integer is the CPU's term in units of 1 / scaler^2. `sample_square<T>()`
selects it for `uint16_t` samples and keeps the integer square for `uint8_t`,
where the two are the same number. The reduction, the readback and the host's
two divisions stay as they are.

Every term is a multiple of the unit. The CPU's running `double` sum is
therefore exact, and equal to the device's integer sum, while it is below
2^53 units. A term is below 2^32 units, so that holds for every frame of up
to 2^21 = 2 097 152 pixels at any content (1920x1080 has 2 073 600) and for
every frame at 8, 10 and 12 bits.
`scripts/ci/exact_twins.d/float_moment.cuda` declares the twin exact for that
range.

On a 16-bit frame of more than 2^21 pixels whose second moment times the
pixel count reaches 2^37 the CPU's sum passes 2^53 units. From there the CPU
rounds every add of a term that is not a multiple of the sum's last place,
and the twin, which holds the exact sum and rounds once, can differ from it
by at most the bound ADR-1447 derives,

    (pixels - 2^21 + 1) / pixels * 2^(e - 69) + 2^-37

with `e` the binade of the sum in units. `test_cuda_float_moment_parity`
asserts equality in the exact range and this bound past it, through the cases
of `core/test/float_moment_twin_parity.h` that the SYCL test already uses.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add the float square as an integer (this ADR) | The CPU's term; the sum stays an exact integer, so the block reduction and its order do not matter; no change to the host; no measurable cost; the design of the HIP and SYCL twins | Not the CPU's bits past 2^53 | Chosen |
| The float square for every sample type | One expression | Changes the 8-bit kernel's machine code for a value that is the same number | The 8-bit kernel stays as it is |
| A plain `sample * sample` | Shorter | Every other exact CUDA kernel spells each rounding as an `_rn` intrinsic, so that no compiler option can change it; the fatbins are built without FMA contraction (ADR-1403), but the intrinsic is the contract | Consistency with the other twins |
| Reproduce the CPU's sequence of roundings past 2^53 with `ordered_sum.h` ([ADR-1433](1433-cuda-ssimulacra2-cpu-sum-order.md)) | Bit-identical on every frame | Four kernels and a walk per sum for 16-bit frames above 2 097 152 pixels with a mean square above 2^37 / pixels | Deferred with the other twins: `T-HIP-FLOAT-MOMENT-PAST-2-53-2026-10-02`. The bound is stated and tested |
| Keep the exact squares and a tolerance | No change | 1.0e-4 off at 16 bits, outside the 5e-5 gate tolerance | A defect |

## Consequences

- **Positive**: measured on an RTX 4090 at `--precision max`, the four
  outputs of every frame equal `--backend cpu` on all 262 frames measured:
  the 250 frames of the table above and 12 frames of 40x40 to 64x64 noise
  (1048 outputs; before, the second moments of all 77 16-bit frames with real
  low bits differed). 17 of the 32 16-bit 3840x2160 frames are past 2^53
  units and identical too.
- **Positive**: no measurable cost; the numbers are in
  [the CUDA backend page](../backends/cuda/overview.md#float_moment_cuda-matches-the-cpu-float_moment-at-16-bits-2026-10-02).
- **Negative**: stored 16-bit `float_moment_cuda` second moments change by up
  to 1.0e-4.
- **Neutral / follow-ups**:
  - Past 2^53 the twin is within the bound above and not bit-identical in
    general: 2.7e-7 on the test's 2560x1440 frame (bound 6.6e-6). The parity
    gate script compares an exact cell at 0 for every frame; it has no
    per-frame range, so a 16-bit fixture in that range would have to be added
    to it with this bound. `T-HIP-FLOAT-MOMENT-PAST-2-53-2026-10-02` covers
    every twin.
  - `float_moment_metal` stays in
    `T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`.
  - `test_hip_float_moment_parity.c` carries its own copy of the fixtures and
    the bound that `float_moment_twin_parity.h` holds for the SYCL and CUDA
    tests; moving the HIP test onto the header is left to the RC5 dedupe pass.
  - Guards: `test_cuda_float_moment_parity` and `_large` (noise at 8, 10, 12
    and 16 bits and a bright 16-bit 1920x1080 frame with `==`; the 16-bit
    cases fail on the old twin; a 2560x1440 frame past 2^53 against the
    bound) and `test_cuda_float_moment_exact_contract.py` (five planted
    regressions, no device).

## References

- `req` (coordinator brief for the CUDA lane, 2026-10-02): "Already known from the HIP lane's cross-check on the 4090: `float_moment_cuda` is 2.8e-5 / 1.0e-4 off at 16 bit (integer squares instead of the CPU's float squares; fixed for HIP in #1789 / ADR-1447 and SYCL in #1793 / ADR-1449, incl. the derived bound past 2^53: reuse their header and bound)".
- [ADR-1447](1447-hip-float-moment-cpu-float-squares.md),
  [ADR-1449](1449-sycl-float-moment-cpu-float-squares.md),
  [ADR-1212](1212-gpu-moment-bit-depth-normalisation.md),
  [ADR-1392](1392-cuda-integer-reductions-one-atomic-per-block.md),
  [ADR-1403](1403-cuda-strict-fp-every-kernel.md),
  [ADR-1421](1421-rc3-rc8-candidate-map.md),
  [ADR-1428](1428-exact-twins-fragments.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md).
- `docs/state.md`: `T-CUDA-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`,
  `T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`,
  `T-HIP-FLOAT-MOMENT-PAST-2-53-2026-10-02`.
