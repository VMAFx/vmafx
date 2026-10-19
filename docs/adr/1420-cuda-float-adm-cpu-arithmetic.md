<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1420: `float_adm_cuda` computes the CPU's arithmetic, divides through the host's reciprocal estimate and returns the CPU's scores bit for bit

- **Status**: Accepted (Superseded-in-part 2026-10-06 by [ADR-1442](1442-float-adm-reference-divides.md) for the probed reciprocal model of float ADM)
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `cuda`, `gpu-parity`, `numerics`, `float-adm`, `testing`, `ci`, `rc3`, `fork-local`

## Context

After [ADR-1403](1403-cuda-strict-fp-every-kernel.md) took FMA contraction out
of every CUDA kernel, `float_adm_cuda` matched the CPU extractor on 144 of 791
measured scores and was up to 1.3e-5 from it (`adm_scale0` on BBB 3840x2160).
Starting from a twin that reproduces `--backend cpu` and putting the old
constructs back one at a time found nine differences
([Research-1420](../research/1420-cuda-float-adm-cpu-arithmetic.md)); all nine
together give the old twin's output bit for bit.

1. **The angle test.** `adm_angle_flag_s()` compares
   `(u.v)^2 >= cos_1deg_sq * o_mag_sq * t_mag_sq`, which C evaluates as
   `(cos^2 * |o|^2) * |t|^2`. The kernel computed `cos^2 * (|o|^2 * |t|^2)`.
   The two products differ in the last bit, the test flips for vectors about
   one degree apart, and a flipped sample switches between the restored and
   the gain-limited value. Alone: 1.3e-5.
2. **The order of the sums.** `adm_csf_den_scale_s()` and `adm_cm_s()` add a
   row into one `float` per band and the rows into another. The kernel
   reduced a row in 256 strided partial sums and a warp tree, and the host
   added the rows in `double`. Alone: 4.4e-7.
3. **The CSF weights.** The host kept a copy of `dwt_quant_step()` that
   rounded `r` and the logarithm to `float` where `adm_tools.h` keeps them in
   `double`. Four of the eight default weights were one to three units in the
   last place off. Alone: 2.1e-7.
4. **The division.** On x86 with SSE2 `adm_tools.c` computes `t / o` as
   `t * rcp_s(o)`, where `rcp_s()` refines the processor's `RCPSS` estimate
   with one Newton step (`ADM_OPT_RECIP_DIVISION`). The result is not the
   IEEE quotient for 2.6 million of the 8.4 million mantissas. The kernel
   divided. Alone: 1.3e-7.
5. **The masking threshold.** `adm_cm_thresh3x3_s()` forms one nine-term sum
   per band, the centre tap fifth, and adds the three. The kernel added the 24
   neighbours and then the three centres into one accumulator. Alone: 9.4e-8.
6. **`FLOAT_ONE_BY_15` and `FLOAT_ONE_BY_30`** are double literals, so the
   centre tap enters its sum as an fp64 addend and the 1/30 product is fp64
   rounded once. The kernel used fp32 constants. Alone: 7.2e-8 and 1.5e-10.
7. **The enhancement gain limit** is a `double` and `rst * adm_enhn_gain_limit`
   an fp64 product. The kernel took a `float`. Nothing at the default 100;
   1.0e-7 at 1.2.
8. **The floor of the frame sums.** `compute_adm()` zeroes a numerator or
   denominator below `1e-10 * area / 1080p`. The host used `1e-2`, a branch
   the CPU source removed as never built. On a flat frame with one sample one
   16-bit level up and no noise floor the CPU reports `adm2 = 0`, the twin
   reported 1.

The fourth is a property of the reference, not of the twin. `RCPSS` is
specified by a relative error bound (1.5 * 2^-12), not bit for bit, and the
manufacturers' implementations differ in the low bits (see References). After
one Newton step the low bits of the reciprocal still depend on which estimate
it started from. The CPU extractor's scores are therefore those of the
processor it runs on, and a twin that is to equal `--backend cpu` has to
reproduce that processor's estimate. Only one processor was measured here (a
Ryzen 9 9950X3D).

The CPU extractor is the reference and its golden assertions are not modified
([ADR-0024](0024-netflix-golden-preserved.md)). The maintainer's direction for
the remaining CUDA twins is results first, bit for bit, speed afterwards.

## Decision

We will make `float_adm_cuda` return the CPU extractor's values bit for bit:
the reference's per-sample arithmetic in the reference's types, the host
processor's reciprocal estimate, the reference's running sums, and the
reference's own routines for everything the host concludes.

**The reference's arithmetic, written once.**
`core/src/feature/cuda/float_adm/float_adm_device.h` holds
`adm_angle_flag_s()`, `adm_decouple_band_s()`, `adm_csf_s()`,
`adm_cm_thresh3x3_s()` and the terms of the three reductions operation for
operation. The gain limit is a `double` kernel argument, the two CSF
constants are double literals, the clamps are the reference's ternaries. On
the device every rounding is an explicit `__fmul_rn` / `__fadd_rn` /
`__fsub_rn` / `__fdiv_rn` / `__dmul_rn` / `__dadd_rn`; the host compiles the
same header for a test.

**The host's reciprocal estimate, as a table.** On this class of processor
the estimate of a normal `x` is a function of the top 12 mantissa bits, scaled
by the exponent; zeros and denormals give an infinity, infinities and results
below the normal range a zero. `adm_reciprocal_model_probe()`
(`core/src/feature/adm_reciprocal_model.c`) fills the 4096-entry table from
the processor's own instruction and then proves the model against it: every
one of the 2^23 mantissas at one exponent, every exponent and both signs at
2 048 mantissas each, the special values. The decouple kernel evaluates
`adm_reciprocal_model_bits()` in integer arithmetic and applies the
reference's Newton step. Three modes cover every host:

| Mode | When | Device |
|---|---|---|
| `ADM_DIVISION_RECIPROCAL_TABLE` | the probe proves the table model | table estimate, Newton step |
| `ADM_DIVISION_IEEE` | the CPU build divides (no SSE2 macro: MSVC, ARM) | `t / o` |
| `ADM_DIVISION_RECIPROCAL_IEEE` | the model fails | IEEE reciprocal, Newton step; exact when the host's estimate is the IEEE reciprocal (an emulator), otherwise the extractor logs that it is close to the CPU, not equal |

**Terms, then row sums, then the rows.** `float_adm_decouple_csf` writes the
CSF of the additive and of the restored signal for every sample.
`float_adm_terms` stores nine terms per sample of the reduced region: the
denominator, the adm2 numerator and the AIM numerator, three bands each.
`float_adm_row_sums` runs one thread per row and slot and adds that row left
to right into one fp32 accumulator. The readback is nine floats per row per
scale, and the host adds them top to bottom, again in fp32. Nothing reduces
per warp or per block.

**The reference's own routines on the host.** `adm_tools.c` exports what the
twin had copied (`core/src/feature/adm_float_reference.h`):
`adm_csf_rfactor_s()` for the weights, `adm_border_s()` for the reduced
region, `adm_decouple_cos_1deg_sq_s()` for the angle threshold, and
`adm_pool_bands_s()`, the root-plus-noise-floor tail that four reductions of
`adm_tools.c` spelled out and now call. The frame sums are floored at the
reference's `1e-10`.

**Gate.** `float_adm` gets a `cuda` entry in `EXACT_TWINS`
(`scripts/ci/cross_backend_calibration.py`): the CPU ↔ CUDA cell is compared
with tolerance 0 at `--precision max`. The other `float_adm` twins keep
places=4.

**What stays inexact.** `adm_p_norm` other than 3 raises every term with
`powf()`, on the CPU glibc's and on the device CUDA's. With `adm_p_norm` 2,
4.5 and 20 the scores are within 1.1e-7 of the CPU (20 to 72 of 791
differ); `adm_p_norm = 1` and the default 3 are identical.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the IEEE quotient and tighten the tolerance to the measured bound | No table, no host probe | The cell stays a tolerance; the residual (1.3e-7 here) is a property of the host processor, so the bound is not one number | The direction is bit for bit, and the estimate can be reproduced |
| A fixed table of one processor's estimates in the kernel | No probe | Wrong on every other processor; the CPU extractor on an Intel host does not return an AMD host's scores | The reference is the CPU extractor of the host that runs the comparison |
| Turn `ADM_OPT_RECIP_DIVISION` off so the CPU divides | Host-independent reference, twins need no table | Changes the CPU's float ADM scores, which the Netflix golden assertions pin | [ADR-0024](0024-netflix-golden-preserved.md); recorded as `T-FLOAT-ADM-RECIPROCAL-ESTIMATE-HOST-DEPENDENT-2026-10-01` |
| Probe a sample of mantissas instead of all | 9 ms less at init | A table that is nearly right gives scores that are nearly right, silently | The full sweep is 9.4 million calls once per extractor |
| Read every term back and add on the host | No row-sum kernel | 48 MB of readback per 3840x2160 frame | One 128-thread-per-block kernel does it on the device |
| Keep the copied `dwt_quant_step()`, corrected to `double` | Smaller diff | A second copy of the weights, which is how they drifted | HISS-19: the twin calls `adm_csf_rfactor_s()` |
| Reproduce glibc's `powf()` on the device for `adm_p_norm != 3` | The option would be exact too | glibc's result depends on how the host's libm was built (FMA or not), as [ADR-1380](1380-cuda-speed-device-resident-pipeline.md) found for `log2f` | A non-default option within 1.1e-7; documented instead |

## Consequences

- **Positive**: measured on an RTX 4090 at `--precision max` against master
  `5c8b9e9c7`, every output of every frame equals `--backend cpu`: Netflix
  576x324 at 8 bits (48 frames) and at 10, 12 and 16 bits (3 frames each),
  both 1920x1080 checkerboard pairs (3 frames each) and BBB 3840x2160 (50
  frames): 791 of 791 scores (`adm2`, `adm_scale0..3`, `aim`, `adm3`), and
  2 034 of 2 034 outputs with `debug=true` (the frame sums and the eight
  per-scale sums included). Before: 144 of 791, largest difference 1.3e-5,
  and 658 of 2 034. The gate over all 200 BBB frames reports 0. The same
  holds when clang's CUDA driver builds the kernels, and with non-default
  `adm_enhn_gain_limit`, `adm_bypass_cm`, `adm_noise_weight`,
  `adm_skip_aim_scale`, `adm_norm_view_dist`, `adm_ref_display_height` and
  the `adm3` options.
- **Positive**: the twin no longer reports `adm2 = 1` where the CPU reports 0
  for content whose frame sums fall between the two floors.
- **Positive**: one launch fewer per scale (five instead of six), and the
  decouple is computed once per sample instead of once per stage.
- **Negative**: the kernels of one instance cost 0.76 ms per 3840x2160 frame
  before and 1.11 ms after (median of five alternating runs of nine instances
  against one, 50 frames, host load average 6). A single `float_adm_cuda` run
  at 4K is 1.87 and 1.98 ms per frame (seven alternating pairs, paired
  difference +0.12, quartiles -0.06 to +0.26).
  `T-CUDA-FLOAT-ADM-EXACT-THROUGHPUT-2026-10-01`.
- **Negative**: 48 MB more device memory at 3840x2160 (nine floats per sample
  of the reduced scale-0 region), and about 10 ms more at extractor start for
  the probe.
- **Negative**: stored `float_adm_cuda` outputs change by up to 1.3e-5.
- **Neutral / follow-ups**:
  - The CPU extractor's float ADM scores depend on the host processor's
    `RCPSS`. The twin follows whatever host it runs on; the reference itself
    is recorded as `T-FLOAT-ADM-RECIPROCAL-ESTIMATE-HOST-DEPENDENT-2026-10-01`.
  - `adm_tools.c` changes in three places, none in its arithmetic: four
    reductions call `adm_pool_bands_s()` instead of repeating its three
    lines, `adm_decouple_s()` reads `cos^2` from a function, and `rcp_s()`
    reads the estimate from `rcp_estimate_s()`. CPU scores over the fixtures
    and twelve option sets are unchanged (4 752 of 4 752 outputs).
  - The contract mirrors `adm_tools.c`. A change to the decouple, the CSF,
    the masking threshold or a reduction changes `float_adm_device.h` in the
    same PR. `test_float_adm_device_math` compares the header with those
    routines on the host and fails when they diverge;
    `test_cuda_float_adm_exact_contract.py` pins the design at source level.
  - The reference is the contraction-free CPU build. x86-64 has no FMA in its
    baseline, so `adm_tools.c` cannot contract there; the device-free test is
    registered for x86-64.
  - Frames below 17x17 are not covered: the CPU extractor reads outside its
    coarsest bands there (`T-FLOAT-ADM-TINY-FRAME-BAND-READS-2026-10-01`).
    The twin stays inside its buffers and equals the CPU from 17x17 up.
  - `float_adm_sycl`, `float_adm_hip` and `float_adm_metal` carry the same
    arithmetic the CUDA twin had: `T-GPU-FLOAT-ADM-CPU-ARITHMETIC-2026-10-01`.

## References

- `req` (maintainer brief, 2026-10-01): "results before speed; a twin
  reproduces the CPU bit for bit, and tuning comes afterwards."
- [ADR-1403](1403-cuda-strict-fp-every-kernel.md),
  [ADR-1412](1412-cuda-float-vif-cpu-arithmetic.md),
  [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md),
  [ADR-1380](1380-cuda-speed-device-resident-pipeline.md),
  [ADR-1220](1220-gpu-float-adm-options-reach-kernels.md),
  [ADR-1214](1214-float-adm-csf-scale-watson-mode-and-aliases.md),
  [ADR-1204](1204-adm-cm-edge-clamp-gpu-twins.md),
  [ADR-0574](0574-hdr-features-cuda-twins-phase-1.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-0024](0024-netflix-golden-preserved.md).
- [Research-1420](../research/1420-cuda-float-adm-cpu-arithmetic.md).
- Intel 64 and IA-32 Architectures Software Developer's Manual, volume 2,
  `RCPSS` (December 2023 text, read at
  <https://www.felixcloutier.com/x86/rcpss>): the relative error of the
  approximation is at most 1.5 * 2^-12; a denormal source is treated as a
  zero; tiny results are flushed to zero; and for inputs between two stated
  bounds near 2^126 whether the result is tiny "depend[s] on the
  implementation".
- Bruce Dawson, "Floating-Point Determinism" (2013),
  <https://randomascii.wordpress.com/2013/07/16/floating-point-determinism/>:
  "we should not be surprised that different manufacturers' implementations
  (some mixture of tables and interpolation) give different results in the
  low bits."
