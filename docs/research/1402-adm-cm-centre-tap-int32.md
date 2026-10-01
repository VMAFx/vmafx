<!-- markdownlint-disable MD013 MD060 -->
# Research-1402: Removing the int16 wrap of the integer ADM masking centre tap

- **Status**: Active
- **Workstream**: [ADR-1402](../adr/1402-adm-cm-centre-tap-int32.md)
- **Last updated**: 2026-10-01

## Question

Integer ADM narrows the centre tap of its scale-0 masking threshold to int16. Does any Netflix golden assertion depend on that wrap? If not, which inputs change score when the tap is kept in int32, by how much, what must the vector and device kernels compute to stay bit-identical to the scalar code, and what does the change cost?

## Sources

- `core/src/feature/integer_adm_kernels.h`: `adm_cm_thresh()`, `adm_cm_accum_round()`, `adm_cm_rows()`.
- `core/src/feature/adm_cm_accumulator.h`: `adm_cm_excess_s0()`.
- `core/src/feature/x86/adm_avx2.c`, `core/src/feature/x86/adm_avx512.c`: `cm_thresh_band_*()`, `cm_excess_*()`, `cm_accum_*()`, `cm_row_*()`.
- `core/src/feature/cuda/integer_adm/adm_cm.cu`, `core/src/feature/hip/integer_adm/adm_cm.hip`, `core/src/feature/sycl/integer_adm_sycl.cpp`, `core/src/feature/metal/integer_adm.metal`.
- [Netflix/vmaf#1602](https://github.com/Netflix/vmaf/pull/1602), first revision (SIMD follows the scalar wrap) and second revision (the wrap removed).
- Host: Ryzen 9 9950X3D, RTX 4090 (CUDA), gfx1036 (HIP), Arc A380 under the Linux xe driver (SYCL), gcc 16.2.1, icpx 2026.0. Before: master `2c3acf1c9`. No Apple device.

## Findings

### The golden gate does not depend on the wrap

The gate was run first, with only the scalar change applied and the assembly paths disabled (`-Denable_asm=false`), so that every test took the changed code. Then it was run on the finished change with the default dispatch.

| Build (golden profile of `scripts/ci/setup-golden-build.sh`, gcc) | Result of the five golden test files |
|---|---|
| master, default dispatch | 271 passed, 12 skipped |
| master, scalar only | 271 passed, 12 skipped |
| scalar prototype, scalar only | 271 passed, 12 skipped, same outcome per test |
| finished change, default dispatch | 271 passed, 12 skipped |

The 13 fixture pairs the golden tests read (the `src01` pair at 8, 10, 12 and 16 bits and 4:2:2, both checkerboards, `flat`, `sparks`, two `akiyo` crops, the 160x90 `src01` crop, `KristenAndSara`) give JSON identical to master at `--precision max`: with `--feature adm --feature float_adm` at `--cpumask` 0, 48 and 4294967295, and with the default model on the eleven pairs large enough for it.

### Which inputs change, and by how much

CPU, default options, pooled mean, before to after. The values are the same at `--cpumask` 0, 48 and 4294967295.

| Input | Metric | Before | After |
|---|---|---|---|
| flat grey 64x64 against 4x2 patches `255 0 0 0` every 16 pixels | `integer_adm_scale0` | 1.0829225419556654 | 1 |
| | `integer_adm2` | 1.035481944668303 | 1 |
| flat grey 24x24 against one such patch at (3, 3) | `integer_adm_scale0` | 1.0701309766616138 | 1 |
| | `integer_adm2` | 1.0301034698295983 | 1 |
| independent uniform 8-bit noise, 576x324, 3 frames | `integer_adm2` | 0.38954843646923215 | 0.38950305912838107 |
| | `integer_adm_scale0` | 0.4549724278265123 | 0.45480076976959144 |
| | `integer_adm3` | 0.36814184297691904 | 0.3681193593966255 |
| | `integer_aim` | 0.653264750515394 | 0.65326434033513 |
| the same noise against itself plus uniform [-16, 15] | `integer_adm3` | 0.9771812019019719 | 0.9772170356634652 |
| | `integer_aim` | 7.166752298663627e-05 | 0 |

Unchanged: one-pixel vertical stripes against the same stripes shifted on every third row, salt-and-pepper impulses on a gradient, 6x10 blocks against a horizontal blur of them (all 576x324), and scales 1 to 3 of every input. On the two noise pairs the default model's own ADM features (computed with `adm_csf_mode=2`, which weights the bands differently) and its VMAF score are unchanged as well.

`float_adm` gives 1 on both patch pictures before and after. `integer_aim` on the patch pictures is 3.1756 (64x64) and 2.1640 (24x24) before and after, where `float_adm`'s `aim` is 1; this change does not touch that difference and it was not investigated.

### What "bit-identical to the scalar" requires of the vector kernels

The scalar excess is `clamp(M - T * 2^s, 0, INT32_MAX)` with `M = |x|` and `T` the threshold. In the kernels `x` is an int16 sample times a uint16 factor, so `M < 2^31`, and `T` is a sum of 24 int16 values and three taps of at most 69904, so `|T| <= 996120`. With `s = 12` (diagonal band) the product leaves int32 from `|T| = 2^19`.

- `T >= 0` and `T * 2^s < 2^31`: `M - (T << s)` is exact in int32 and only the clamp at 0 can act. This is the old 32-bit expression. Every decoded picture is in this case: its filtered neighbours are non-negative, and reaching `2^19` needs an average of 19418 per term.
- `T >= 2^(31 - s)`: the result is 0. The 32-bit expression wraps the product and returns a positive excess.
- `T < 0`: the result is `min(M + |T| * 2^s, INT32_MAX)`. With `|T|` clamped to `2^(31 - s)` the sum is exact in uint32.

The vector rows therefore sum a row with the 32-bit expression and OR its thresholds together; when a bit of `~(2^19 - 1)` is set in that OR (a negative threshold or one of at least `2^19`), the row is summed again with the exact form: threshold clamped to `+/-2^(31 - s)`, signed `max(d, 0)` for a non-negative threshold, unsigned `min(d, INT32_MAX)` for a negative one. A branch per block on the same test left the AVX-512 stage 7% slower than before the change; the per-row form leaves it 4% to 5% slower (see the stage times).

Upstream's second revision uses a different vector form: lanes with `T > (INT32_MAX >> s)` are zeroed, the rest take the 32-bit expression. That equals upstream's scalar for `T >= 0` only. Planted into `cm_excess_avx2()` it fails `test_integer_adm_simd` (67x21, negative block: `0x1.211a32p+11` against the scalar's `0x1.211a3p+11`).

The scalar narrows the squared excess to int32, so for an excess of `2^30` or more the cube is negative and its shift is arithmetic. AVX-512 has `_mm512_sra_epi64`. AVX2 has no 64-bit arithmetic shift; an arithmetic shift of `a` by `n` equals the logical shift of `a + 2^63` minus `2^63 >> n`, modulo `2^64`, so the kernel adds `2^63` to the rounding term, accumulates the biased terms, and subtracts `lanes * (2^63 >> n)` from the row total. That costs no instruction per sample; upstream's variable-count `sra_epi64` helper costs five per shift.

### Planted defects

`test_integer_adm_simd` compares `adm_cm_avx2()` and `adm_cm_avx512()` with the scalar kernels on hand-built bands: dense fills with filtered bands of either sign and with non-negative ones, one full-range event at every position, and 3x4 blocks of -32768 or 32767 in the filtered bands at every column. Each of these defects, planted alone, fails it:

| Defect | AVX2 | AVX-512 |
|---|---|---|
| the row never takes the exact pass | fails | fails |
| the exact pass is triggered by the sign bit only | fails | fails |
| exact form without the threshold clamp | fails | fails |
| exact form without saturation of a raised excess | fails | fails |
| int16 wrap of the centre tap restored | fails | fails |
| tail block lanes off by one | fails | fails |
| tail block dropped | n/a | fails |
| tail block thresholds left out of the row's OR | fails | n/a |
| cube shift logical (no bias) | fails | n/a |
| bias count one block short | fails | n/a |

`test_integer_adm_cm_threshold` (flat reference against patches, every dispatch level) fails on master with `64x64 cpumask 4294967295 frame 0 integer_adm_scale0: 1.0829225419556654`.

### The undefined shift in the x86 tails

gcc 16.2.1, `-Db_sanitize=address,undefined`, 24x24 flat grey against one patch at (3, 3), `vmaf --feature adm --no_prediction`:

- master: `adm_avx512.c:2291:17: runtime error: left shift of negative value -15176` (also lines 2293 and 2295); with `--cpumask 48`, `adm_avx2.c:2687:17` (also 2689 and 2691).
- after: no report at `--cpumask` 0, 48 and 4294967295. The thirteen ADM unit tests pass under meson's `UBSAN_OPTIONS=halt_on_error=1`.

### Device twins

Before is a build of master `2c3acf1c9`, after a build of the change; 20 pairs (the 13 fixture pairs and the seven synthetic ones above), `--precision max`.

| Twin | Before against after | After against the scalar CPU | Tests |
|---|---|---|---|
| `adm_cuda`, RTX 4090 | 13 fixture pairs identical; the two patch pairs and the two noise pairs move by the CPU's amounts | within 2.56e-7 on 18 pairs, identical on 2 (the residual is the host-side float finalisation, unchanged) | `test_cuda_adm_parity`, `_tiny_frames` (7 cases), `_small_border`, `_wide_rounding` pass |
| `adm_hip`, gfx1036 | 13 fixture pairs identical; patch pairs and independent noise move by the CPU's amounts (the twin emits no `aim`) | identical on 19 pairs; impulses on a gradient differ by up to 3.96e-7 on scales 2 and 3, before and after | `test_hip_adm_parity`, `_tiny_frames` (7 cases), `_small_border`, `_wide_rounding` pass |
| `adm_sycl`, Arc A380 (xe) | 13 fixture pairs identical; the same four pairs move by the CPU's amounts | identical on all 20 pairs, all seven metrics | `test_sycl_adm_parity`, `_tiny_frames` (7 cases, bit-exact arm), `_parity_large`, `test_sycl_kernel_scratch` (108 kernels audited on master `67169ca4c`; the 2 that use scratch memory are in the ratchet and neither is integer ADM) pass |
| `integer_adm_metal` | not run | not run | source change only |

`vmaf --backend hip --feature adm_hip --threads 4` exits with `problem flushing context` on this gfx1036, on master and after alike; the HIP runs above use no worker threads.

### Stage times

Old and new objects linked into one benchmark and alternated on the same frames (gcc 16.2.1, `-O3`, no LTO; minimum of 4 to 6 runs; milliseconds per call at 1920x1080 8-bit unless noted). The same benchmark with the old source in both slots gives a ratio of 1.000.

| Level | Scale-0 contrast masking, before | after | Whole ADM pipeline, before | after |
|---|---|---|---|---|
| scalar | 2.240 | 2.394 (+7%) | 22.77 | 23.19 (+1.8%) |
| AVX2 | 0.565 | 0.537 (-5%) | 7.60 | 7.44 (-2.1%) |
| AVX2, 576x324 | 0.057 | 0.050 (-12%) | 0.577 | 0.553 (-4.2%) |
| AVX-512 | 0.240 | 0.253 (+5%) | 3.19 | 3.27 (+2.6%) |
| AVX-512, 576x324 | 0.024 | 0.024 | 0.304 | 0.310 (+2.0%) |

The scalar excess was first written with an early return for a masked sample. On noise the branch mispredicts: the scalar AIM contrast-masking stage took 4.40 ms against 2.30 ms. Two selects removed that. The split x86 files at first ran the leftover columns of a row through the shared scalar kernel, which cost the AVX-512 stage 33% at 576x324 (8 leftover columns per row); the leftover columns are now the top lanes of one more vector block.

The AVX-512 pipeline as a whole is 0% to 5% slower than before the split (typically 3%) at 0.2% to 0.5% more instructions (`perf`, `cycles:u` and `instructions:u`); the AVX2 pipeline executes 7% fewer instructions. The AVX-512 difference is tracked as `T-ADM-AVX512-SPLIT-STAGE-TIME-2026-10-01` in `docs/state.md`. An end-to-end `vmaf --feature adm` run on 60 frames of BBB 3840x2160 does not resolve any of these differences on this host while other jobs run: repeated runs of the same two binaries ranged from 6% slower to 6% faster.

### Found on the way, not changed here

- With a non-integer `adm_enhn_gain_limit` the AVX2 and AVX-512 decouple kernels round `rst * gain` where the scalar truncates (`T-ADM-DECOUPLE-X86-FRACTIONAL-GAIN-ROUNDING-2026-10-01`).
- Upstream's AVX2 scale 1-3 cube shift (`i4_cm_cube_avx2()`) is logical where the scalar's is arithmetic. The two differ only for an excess of 2^30 or more, which the scale 1-3 extractor inputs do not reach; it is upstream's code and was left.

## Reproduce

```bash
# golden gate
make test-netflix-golden

# the patch pictures, every dispatch level, and the vector kernels against the scalar
meson test -C build test_integer_adm_cm_threshold test_integer_adm_simd

# before / after on a patch picture (draw it as core/test/test_integer_adm_cm_threshold.c does)
vmaf -r flat_64x64.yuv -d patches_64x64.yuv -w 64 -h 64 -p 420 -b 8 \
     --feature adm --feature float_adm --no_prediction --precision max --json

# device twins
meson test -C build-cuda test_cuda_adm_tiny_frames
meson test -C build-hip test_hip_adm_tiny_frames
meson test -C build-sycl test_sycl_adm_tiny_frames test_sycl_kernel_scratch
```
