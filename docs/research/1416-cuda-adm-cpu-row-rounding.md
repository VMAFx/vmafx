<!-- markdownlint-disable MD013 MD060 -->
# Research-1416: Why adm_cuda was not the CPU's adm — a copied CSF weight routine, a per-warp fold and an fp32 shift

- **Status**: Active
- **Workstream**: [ADR-1416](../adr/1416-cuda-adm-cpu-row-rounding.md), [ADR-1403](../adr/1403-cuda-strict-fp-every-kernel.md), [ADR-0214](../adr/0214-gpu-parity-ci-gate.md)
- **Last updated**: 2026-10-01

## Question

The integer ADM extractor is integer arithmetic until its last step, yet
`adm_cuda` matched `--backend cpu` on a quarter of its outputs and was up to
2.1e-7 away (Research-1403). Which values differ, why, and can the twin be
made identical?

## Sources

- CPU: `core/src/feature/integer_adm.c`, `core/src/feature/integer_adm_kernels.h`
  (contexts, row drivers, result routines; the AVX2 and AVX-512 twins call the
  same result routines), `core/src/feature/adm_cm_accumulator.h`.
- CUDA: `core/src/feature/cuda/integer_adm_cuda.c`,
  `integer_adm/adm_csf_den.cu`, `integer_adm/adm_cm.cu` at master `b22ad4e1a`
  (before) and on `fix/cuda-adm-cpu-arithmetic` (after).
- Upstream: Netflix/vmaf `6ec23e8f2`, `libvmaf/src/feature/integer_adm.h`.
- Host `zeus`: RTX 4090 (sm_89, driver 615.71.09), CUDA 13.4 (`nvcc`
  V13.4.92), gcc 16.2.1, glibc 2.44. `meson setup build-cuda core
  -Denable_cuda=true -Denable_sycl=false --buildtype=release -Db_lto=false`.
- Fixtures, 4:2:0, `--precision max`: Netflix `src01_hrc00/01_576x324` at 8
  bits (48 frames) and 10, 12, 16 bits (3 frames each), the two 1920x1080
  checkerboard pairs (3 frames each), BBB 3840x2160 (50 frames).

## Findings

### 1. Before: which outputs differ

Identical outputs of all outputs (the seven scores `integer_adm2`,
`integer_aim`, `integer_adm3`, `integer_adm_scale0..3` of every frame) and
the largest difference:

| Fixture | Before | After |
|---|---|---|
| Netflix 576x324, 8-bit, 48 frames | 67/336, 1.85e-7 | 336/336, 0 |
| Checkerboard 1 px, 3 frames | 0/21, 1.13e-7 | 21/21, 0 |
| Checkerboard 10 px, 3 frames | 2/21, 1.16e-7 | 21/21, 0 |
| BBB 3840x2160, 50 frames | 107/350, 2.09e-7 | 350/350, 0 |
| Netflix 576x324, 10-, 12-, 16-bit, 3 frames each | 3/21 each, 1.33e-7 | 21/21 each, 0 |

With `debug=true` the per-scale sums are published too. Before, on the
Netflix pair: `integer_adm_num_scale0` identical on every frame,
`integer_adm_num_scale1..3` and `integer_adm_den_scale0..3` one or two fp32
units apart on most frames (in the first eight frames always with the twin
the larger). After: 2 034 of 2 034 outputs over all seven fixtures.

### 2. The raw accumulators of one frame

A temporary `fprintf` in the CPU's result routines and in the twin's host
conclusion, Netflix pair, frame 0:

| Value | CPU | CUDA before |
|---|---|---|
| CSF weight scale 0, h/v and d | `0x1.1cc772p-6`, `0x1.820d4ep-8` | `0x1.1cc774p-6`, `0x1.820d54p-8` |
| CSF weight scale 1, h/v | `0x1.060508p-5` | `0x1.06050cp-5` |
| CSF weight scale 3, h/v and d | `0x1.762816p-5`, `0x1.00839p-5` | `0x1.762816p-5`, `0x1.008392p-5` |
| CM accumulator scale 0 (h, v, d) | 419962341045611, 261303835715446, 14767938623076 | the same |
| CM accumulator scale 1, h | 15190347313023 | 15190357973544 |
| CM accumulator scale 3 (h, v, d) | 10812633704571, 7511360892716, 327739156266 | 10812633704571, 7511360892716, 327739267601 |
| Denominator accumulator scale 0 | 54536897583245, 35312382394752, 5547938147371 | the same |
| Denominator accumulator scale 1 (h, v, d) | 143023480495444, 110707684076867, 23501727673189 | 143023480495450, 110707684076865, 23501727673181 |

Two patterns:

- Wherever the CSF weight differs, so does the contrast-masking
  accumulator (scale 1 h; scale 3 d only), by 7e-7 relative. Scale 0 is
  untouched: its weights are converted to 16-bit fixed point (36453, 36453,
  49417) and both versions round to the same integers. Scales 1 to 3 convert
  to 32 bits and keep the difference.
- The denominator accumulators of scale 1 differ by a few units although
  their input (the reference DWT) is identical.

### 3. Cause 1: a copy of the CSF weight routine

`integer_adm_cuda.c` had its own `dwt_quant_step()` and
`adm_csf_factors()`. The CPU's (`integer_adm_kernels.h`) reads

```c
float Q = 2.0 * params->a * pow(10.0, params->k * (double)temp * temp) / ...
```

and the copy `params->k * temp * temp`, a `float` product. The `(double)`
came with #552 (a CodeQL sweep, 2026-05-09); upstream Netflix has the
`float` form. So the twin computed upstream's weights and the CPU the
fork's.

With the copy deleted and nothing else changed (old denominator kernels, old
host conclusion), all five standard fixtures are identical: 1 926 of 1 926
outputs with `debug=true`. Cause 1 is the whole distance measured there.

### 4. Cause 2: the denominator was folded per warp

`adm_csf_den_fold()` rounds a row once:
`accum += (inner + add_shift_accum) >> shift_accum`. The kernels launched
several blocks of 128 threads per row, and every warp leader folded its own
partial sum. `shift_accum` is `ceil(log2(rows))` at scales 1 to 3 and
`ceil(log2(area) - 20)` at scale 0, so bits are discarded at each fold and
the result depends on where it happens (the accumulators in finding 2).

The float conclusion divides the accumulator, multiplies by the cubed
weight and takes a cube root, which hides a few units in 1e14. It does not
hide them when the accumulator is small. With the CPU's weights and the old
kernels, a 640x360 frame of mid grey with one sample in 200 raised by one
level, against the same frame with half the samples raised by one more:

| Output | CPU | CUDA (old fold) | Difference |
|---|---|---|---|
| `integer_adm_den_scale2` | 12.870977401733398 | 12.870979309082031 | 1.9e-6 |
| `integer_adm_den_scale3` | 8.5786800384521484 | 8.57867431640625 | 5.7e-6 |
| `integer_adm_scale3` | 0.98982246255251849 | 0.98982312277219264 | 6.6e-7 |
| `integer_adm2` | 0.99606754716652557 | 0.99606759879178142 | 5.2e-8 |

Other low-detail frames tried: the same dots show it at 1920x1080 and not at
256x144, a 16-pixel checker of two levels shows it at 640x360 only, and a
two-level step at none of the three sizes.

### 5. Cause 3: the scale-0 shift was an fp32 logarithm on the device

The scale-0 kernel computed
`__float2uint_ru(__log2f((bottom - top) * (right - left)) - 20)`; the host
concluded with `ceil(log2(area) - 20)` in `double`, as the CPU does. Both
formulas evaluated on the device and the host for every area from 1 to
2^26: they disagree for 81 areas, each just above a power of two
(1 048 577, 2 097 153, 2 097 154, 4 194 305 to 4 194 309, ...), where fp32
rounds the logarithm down to the integer. The kernel then shifts by one bit
less than the host assumes and the denominator is twice too large.

A frame that hits it: 962x13542 has a scale-0 band of 481x6771 and a border
region of 387x5419 = 2^21 + 1 samples.

| Output | CPU | CUDA before | CUDA after |
|---|---|---|---|
| `integer_adm_den_scale0` | 260.0687561035156 | 296.22802734375 | 260.0687561035156 |
| `integer_adm_scale0` | 0.9790734684255321 | 0.859562214117938 | 0.9790734684255321 |
| `integer_adm2` | 0.9785642491651569 | 0.9650423056507962 | 0.9785642491651569 |
| `integer_adm3` | 0.9881823527375762 | 0.9814365778143603 | 0.9881823527375762 |

The other device-derived shifts, `ceil(log2(w))` and `ceil(log2(h))` in
`adm_cm.cu` and in the old scale 1 to 3 denominator kernel, take an integer
extent, where fp32 is enough: the device result equals the CPU's for every
extent from 1 to 131 071. `adm_cm.cu` is therefore unchanged. The host's
float border formula (`w * (float)0.1 - 0.5f`) and `ceilf(log2f(w))` were
compared with the CPU's `double` forms for every extent up to 131 072 as
well: no difference.

### 6. Cause 4: the `adm_skip_scale0` seeds

`integer_adm_scale0()` returns with `sc->den = 1e-10` (a `float` member) and
a zero numerator, and the driver adds both to the frame sums. The twin
stored `1e-10` as a `double` for the per-scale output and added nothing:
`integer_adm_den` 1.0e-10 lower, `integer_adm2` 2.8e-13 away.

### 7. Options

After the change, identical on the Netflix pair and the 1 px checkerboard
(18 outputs with `debug=true`): `adm_csf_mode` 1, 2 and 3;
`adm_p_norm=2.5` with `adm_noise_weight=0.02`; `adm_enhn_gain_limit=1.0`
with `adm_min_val=0.5`; `adm_norm_view_dist=1.5` with
`adm_ref_display_height=2160`; `adm_dlm_weight=0.25` with
`adm_noise_weight=0.3`; `adm_skip_scale0`; `adm_skip_aim`.

One combination fails on both sides in the same way and is not this
change's: `adm_csf_mode=1:adm_csf_scale=1.2` on the 1 px checkerboard. The
vertical-band AIM accumulator of scale 1 comes out negative
(-12038787992729937, where `adm_csf_scale=1.0` gives 101823003751485135),
`powf()` of it is NaN, and the frame is rejected with `undefined or
non-finite aggregate`. `T-ADM-AIM-BARTEN-SCALE-TERM-WRAP-2026-10-01`.

### 8. Cost

BBB 3840x2160, RTX 4090, shared host (load average 12 to 16), before = master
`90aa3f619`:

| Measurement | Before | After |
|---|---:|---:|
| One twin per run, `(t(200) - t(2)) / 198`, nine alternating pairs | 3.63 ms | 3.66 ms (paired +0.01, quartiles -0.02 to +0.22) |
| One more instance, `(t9 - t1) / (8 * 60)`, median of seven runs | 2.47 ms | 2.48 ms |

The denominator kernels now run one block per row instead of one per 1 024
columns of a row, and add one atomic per row instead of four per block.

### 9. The other twins

- `adm_hip` (`hip/integer_adm/adm_csf_den.hip`, read from source, not run):
  every thread folds its own partial sum, and the scale-0 shift is
  `ceilf(log2f((float)area) - 20.f)`. Its host has the `(double)` exponent.
  `T-HIP-ADM-CSF-DEN-FOLD-PER-THREAD-2026-10-01`.
- `adm_sycl` is documented as bit-identical to the CPU (ADR-1362).

## Reproduce

```sh
meson setup build-cuda core -Denable_cuda=true -Denable_sycl=false --buildtype=release -Db_lto=false
ninja -C build-cuda
# every case fails on master: weights (all), fold (sparse), shift (962x13542)
build-cuda/test/test_cuda_adm_parity
# no device needed
build-cuda/test/test_adm_cm_row_rounding
python3 -m pytest core/test/test_cuda_adm_exact_contract.py core/test/test_adm_cm_row_rounding_contract.py
# the gate cell
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary "$PWD/build-cuda/tools/vmaf" \
  --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --features adm --backends cpu cuda
```
