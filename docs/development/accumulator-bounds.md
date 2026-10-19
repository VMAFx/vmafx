<!-- markdownlint-disable MD013 MD060 -->
# Integer accumulator bounds

Every feature extractor adds samples, differences, squares, cubes, counts or
offsets into fixed-width integers. This page records, for every such
integer in every CPU extractor, SIMD path and GPU twin, the largest value
it can reach on the largest pictures VMAFx accepts. It is the result of the
RC3 integer-overflow audit; the per-row tables are on three appendix pages:

- [CPU extractors and SIMD paths](accumulator-bounds-cpu-simd.md)
- [CUDA and HIP twins](accumulator-bounds-cuda-hip.md)
- [SYCL and Metal twins](accumulator-bounds-sycl-metal.md)

A bound here is derived from the code, never guessed: how many terms reach
one accumulator, and the largest term. Each row names the file and line it
was read from (master `571565a47`), so a change to that code is a change to
the row.

## The envelope

| Size | Width x height | Samples per plane (N) | |
|---|---|---|---|
| 8K DCI | 8192 x 4320 | 35,389,440 | |
| 16K | 15360 x 8640 | 132,710,400 | below $2^{27}$ |
| Cap | 32768 x 32768 | 1,073,741,824 | $2^{30}$, `VMAF_PIC_DIM_MAX` per side (`core/src/picture.c`) |

Samples go up to 16 bits (65535; $65535^{2}$ = 4,294,836,225 is below $2^{32}$ but
above INT32_MAX), chroma at full size (4:4:4), and the content is the worst
the arithmetic allows: every sample at the maximum difference, full-range
noise, or a pattern built to drive one term to its maximum. A sum over the
frames of a clip is given as the number of frames after which it wraps.

Each row has one verdict:

| Verdict | Meaning |
|---|---|
| SAFE | The bound is below the type's range at the cap, with the margin stated. |
| OVERFLOW@16K | The type can be exceeded by a picture up to 16K: a defect. |
| OVERFLOW@CAP-ONLY | Safe up to 16K, exceeded between 16K and the cap. |
| DEPENDS | The bound depends on something other than the picture size: an option, the frame count, or samples above $2^{\mathrm{bpc}} - 1$ (no libvmaf entry point checks the range). |

## Verdicts

| Backend group | Rows | SAFE | OVERFLOW@16K | OVERFLOW@CAP-ONLY | DEPENDS |
|---|---|---|---|---|---|
| CPU scalar (with the `core/src/` runtime) | 151 | 137 | 1 | 3 | 10 |
| x86 SIMD (AVX2, AVX-512) | 96 | 81 | 3 | 0 | 12 |
| arm64 SIMD (NEON, SVE2) | 33 | 28 | 0 | 0 | 5 |
| CUDA and HIP | 743 | 730 | 2 | 3 | 8 |
| SYCL | 214 | 203 | 1 | 2 | 8 |
| Metal | 126 | 114 | 1 | 5 | 6 |

The counts are the appendix pages' as read on master `571565a47`, before the
fixes below.

## Defects found

Every row that can overflow is a defect with a row in
[the state ledger](../state.md). Fixed ones keep the score of every input
that did not overflow bit for bit; the fix and the test that fails without
it are named in the row.

| Defect | Backends | Reach | State row |
|---|---|---|---|
| Integer ADM scale-0 contrast-masking row summed in `int64_t` | CPU, AVX2, AVX-512, CUDA, HIP, SYCL | a 31-32 or 63-64 pixel wide picture at the default options (1.044 INT64_MAX); 16K with a CSF weight above 38,400 | `T-ADM-CM-SCALE0-ROW-INT64-OVERFLOW-2026-10-05`, fixed |
| The same row past $2^{64}$ | every backend | a CSF weight between about 45,200 and the ADR-1472 limit of 46,603 | `T-ADM-CM-SCALE0-ROW-UINT64-WEIGHT-BUDGET-2026-10-05`, fixed (scale-0 limit 43,900, ADR-1917) |
| Integer ADM scale 1-3 gain product narrowed to int32 before its bound | CUDA, HIP | any size (an undefined conversion the hardware's saturation hid) | `T-GPU-ADM-S123-GAIN-PRODUCT-NARROWING-2026-10-05`, fixed |
| APSNR clip SSE in `uint64_t` | CPU, CUDA, HIP, SYCL, Metal | frame 33 of 16K, 122 of 8K, 2072 of 1080p at 16 bits and the maximum difference | `T-PSNR-APSNR-CLIP-SSE-UINT64-WRAP-2026-10-05`, fixed |
| `sad_avx512()` took 16-bit differences in signed 16-bit lanes | AVX-512 (a function only its parity test calls) | any size, 16-bit samples that differ by more than 32,767 | `T-SIMD-SAD-AVX512-INT16-DIFFERENCE-2026-10-05`, fixed |
| `vif_tools.c` indexes the prescaled plane with `int` | CPU `float_vif` and SpEED, and the SpEED twins' shared geometry | the cap with `vif_prescale` or `speed_prescale` above 1.414 | `T-PRESCALED-PLANE-INT-INDEX-2026-10-05`, fixed |
| PSNR-HVS prefix scan capped at 32,768 chunks | HIP, SYCL | 4:4:4 from 16384 x 8640 | `T-GPU-PSNR-HVS-SCAN-32768-CHUNKS-2026-10-05`, fixed |
| SpEED covariance divided by the fp32-rounded count | CUDA, HIP, SYCL | `speed_prescale` above 2 past 16K | `T-GPU-SPEED-COV-COUNT-FP32-2026-10-05`, fixed |
| `float_motion` tile load before the plane | CUDA | planes 3 to 9 or 17 samples wide or high (no score effect) | `T-CUDA-FLOAT-MOTION-TILE-READ-BEFORE-PLANE-2026-10-05`, fixed |
| `uint` moment-plane and term indices | Metal | `float_vif` with `vif_prescale` above 2.55 at 16K; four extractors past 16K | `T-METAL-UINT-PLANE-INDEX-2026-10-05`, fixed in code (init refusal); open until the macOS tester re-run |
| Samples above $2^{\mathrm{bpc}} - 1$ wrap integers the CPU keeps wide or truncates | CPU, CUDA, HIP, SYCL, Metal | any size, out-of-range input only | `T-OUT-OF-RANGE-SAMPLES-TWIN-DIVERGENCE-2026-10-05`, closed (contract and opt-in check, ADR-1918) |
| Integer ADM scale-0 CSF magnitude `flt` stored in int16 | CPU and AVX-512 wrap, AVX2 saturates, so AVX2 differs from the scalar code | an h/v CSF weight from 43,900 to the ADR-1472 limit of 46,603 | `T-ADM-SCALE0-CSF-FLT-INT16-WRAP-2026-10-05`, fixed (ADR-1917) |
| Full-mask warp shuffle in a divergent branch; left shift of a negative `long long` | CUDA | any size | `T-CUDA-WARP-REDUCE-UB-2026-10-05`, fixed |

Not defects, recorded as DEPENDS: the CUDA motion batch counter narrows the
frame index to `int` and needs $2^{31}$ frames (414 days at 60 fps). Samples
above $2^{\mathrm{bpc}} - 1$ also make several CPU and SIMD integers wrap (the
16-bit motion `row_sad`, the AVX2 and AVX-512 motion x-convolution, the
integer VIF means the SIMD paths keep un-narrowed, the 16-bit ADM DWT and the
`psnr_hvs` DCT); they are part of the out-of-range row.

### Integer ADM scale-0 rows

The masking reduction adds, per row of a scale-0 band, the cube
`((x^2 + 2^28) >> 29) * x >> shift_cub` of every column, with
`shift_cub = ceil(log2(band width) - 4)`. Compared with itself, a reference
with full-range detail has a zero masking threshold everywhere, so `x` is
the CSF-weighted band value. `scripts/dev/adm_cm_row_bound.py` finds the
largest row exactly, by a search over the column sign patterns of the four
pixel rows that feed one band row, with the library's own integer
arithmetic:

```bash
python3 scripts/dev/adm_cm_row_bound.py --widths 32 64 15360 32768
python3 scripts/dev/adm_cm_row_bound.py --widths 32 --weights 45200 45300 46603
python3 scripts/dev/adm_cm_row_bound.py --pattern 64
```

At the default Watson weights the largest row is 1.044 INT64_MAX at a band
width of 16 (pictures 31 or 32 pixels wide) and 1.021 at 32 (63 or 64),
0.86 at 16K and 0.91 at the cap: below $2^{64}$ everywhere, about half of it.
`core/test/adm_cm_row_overflow_frame.h` is the 64-pixel picture. Under the scale-0 weight limit of 43,900
([ADR-1917](../adr/1917-integer-adm-scale0-weight-limit-csf-magnitude.md))
the largest row at any weight the options can choose is 0.912 of $2^{64}$
(`--weights 43899`, widths 28 to 32).

## How the bounds were checked

Reading the code gives the bound; four runs check that the code does what
the reading says on the largest pictures, with worst-case content:

- **8K exactness matrix.** Every exact CUDA, SYCL and HIP twin against the
  CPU at 8192 x 4320 in 4:4:4, 8 and 16 bits, four frames of worst-case
  content: 135 of 144 cells equal, 9 `n/a` (`cambi` refuses 8K at init,
  `psnr_hvs` refuses 16 bits). See
  [the exact-twin matrix](exact-twin-matrix.md#large-pictures-8k-and-16k).
- **16K CPU matrix.** The CPU extractor of every exact twin at 15360 x 8640
  in 4:4:4, 8 and 16 bits, with the host's AVX-512 / AVX2 dispatch against
  `--cpumask 0xffffffff`: 45 of 48 cells equal, 3 `n/a`.
- **Integer sanitizer.** A clang build with
  `-fsanitize=signed-integer-overflow,unsigned-integer-overflow,shift,pointer-overflow`
  scored the 8K and 16K worst-case fixtures with every CPU extractor, on the
  host dispatch and on the scalar code: 120 runs (19 extractors at 8K, the
  12 whose memory fits at 16K; `psnr_hvs` at 8 bits only). 112 scored and
  the 8 `cambi` runs refused the size at init. There was no signed overflow,
  shift or pointer-overflow report. The 78 unsigned-wrap reports (10 runs,
  27 sites in three files) are modular arithmetic the code relies on: the
  filter border index `i - fwidth / 2` formed in `unsigned` and read as `int`
  (`integer_vif.c`, `x86/vif_avx512.c`), the VIF variance and covariance
  formed as a `uint32_t` difference and read as `int32_t`
  (`integer_vif.c:293-295`; a negative covariance, and a variance that
  rounding takes below 0), and the unbiased shift `OD_UNBIASED_RSHIFT32` of
  the Xiph DCT (`psnr_hvs.c`).
- **`test_accumulator_bounds_16k`** (fast suite) computes the size products,
  shifts and counts the bounds rest on with the library's own helpers at 8K,
  16K and the cap, without a picture of that size: the SpEED submatrix count
  (below $2^{24}$ up to 16K for every prescale), the SpEED tail block, the integer
  ADM row shifts, and the CAMBI window (which passes the reciprocal table above
  4K, so CAMBI refuses 8K and 16K).

Device runs at 16K, device memory limits and the time these sizes take are
not part of this audit; they belong to a later release candidate.

## Adding or changing an accumulator

When you add an integer that sums, counts or indexes over a row, a plane or a
clip, write its row on the appendix page of its backend: the type, the
largest term, how many terms reach it, the bound at 16K and at the cap, and
the verdict. A twin's integer gets the same bound as the CPU's, or the
reason it differs. A sum that can pass its type on accepted input is a
defect: widen it or split it, keep every existing value bit for bit, and
add a test that fails without the change.
