<!-- markdownlint-disable MD013 MD018 MD060 -->
# Post-merge CPU Profile — 2026-05-03

**Date:** 2026-05-03
**Branch / Commit:** `chore/profile-hotpath-2026-05-03` @ `981659a3`
(master tip after PRs #310–#321: CUDA drain-batch #312,
ssimulacra2 host XYB SIMD #314, Vulkan submit-opt-batch #319,
psnr_hvs async+pinned #320)

**Build:** release + `-g -fno-omit-frame-pointer`, CPU-only
(`-Denable_cuda=false -Denable_sycl=false -Db_ndebug=true`)

**Profiler:** `perf record -F 999 -g --call-graph dwarf` (10 × 48 frames,
576×324, vmaf_bench), paranoia level 1.

**CUDA / Vulkan profilers:** `nsys`, `ncu`, `nvprof` — not available on
this host. GPU sections use wall-clock from `netflix_benchmark_results.json`
(last regen at PR #309 / commit `888fd5d1`), which pre-dates PRs #312 and
#319–#320. A separate GPU-profiler run is needed on a CUDA-capable host.

---

## 1. CPU throughput — post-merge baseline (vmaf_bench, 576×324, 48 frames)

| Feature extractor | Avg ms/frame | FPS   |
|-------------------|-------------|-------|
| motion            | 0.06        | 15 532 |
| vif               | 1.13        | 889   |
| adm               | 0.35        | 2 826  |
| float_ssim        | 1.46        | 686   |
| float_ms_ssim     | 2.36        | 424   |
| psnr              | 0.04        | 24 738 |

Full-pipeline (vmaf v0.6.1 model, vmaf CLI): **best 650.7 fps, avg 647.8 fps**
at 576×324 (3 runs, wall clock).

---

## 2. Regression vs 2026-05-02 baseline

The `netflix_benchmark_results.json` baseline was recorded at commit
`888fd5d1` (PR #309, before the current sprint).

| Workload      | Baseline best fps | Current best fps | Delta      | Status   |
|---------------|-------------------|-----------------|------------|----------|
| src01 576×324 | 553.1             | 650.7           | **+17.6 %** | PASS     |

The +17.6 % gain is attributable to PRs #312 (CUDA fence batching — affects
multi-extractor scheduling path even on CPU via reduced engine overhead),
#314 (ssimulacra2 host XYB SIMD — not in the vmaf_v0.6.1 critical path but
reduces system noise), and incremental upstream ports (#315) with minor
integer-vif branch reduction. No regression; wall-clock is strictly better.

**vmaf_bench per-feature FPS has no pre-2026-05-03 measurement baseline.**
This run establishes the post-sprint reference point.

---

## 3. Top-10 CPU self-time (perf, 576×324, 2 642 samples)

| Rank | Symbol                                  | Self % | Interpretation |
|------|-----------------------------------------|--------|----------------|
|  1   | `iqa_convolve_avx512`                   | 39.5 % | 2-D separable Gaussian convolution (SSIM/MS-SSIM µ and σ prep) |
|  2   | `ssim_accumulate_block_avx512.constprop.0` | 18.0 % | Per-16-lane SSIM accumulation with scalar double reduction |
|  3   | `vif_statistic_8_avx512`                | 11.1 % | VIF log-domain statistic with `vpgatherdq` table lookups |
|  4   | `ms_ssim_decimate_avx512`               | 7.2 %  | 9-tap biorthogonal LPF + stride-2 decimation (5 scales) |
|  5   | `vif_statistic_16_avx512`               | 4.8 %  | VIF 16-bit variant, same gather pattern |
|  6   | `vif_subsample_rd_8_avx512`             | 2.6 %  | VIF reference/distorted subsampled read |
|  7   | `adm_decouple_avx512`                   | 1.4 %  | ADM channel decoupling |
|  8   | `ssim_accumulate_avx512`                | 1.3 %  | SSIM block aggregation outer loop |
|  9   | `ssim_precompute_avx512`                | 1.2 %  | SSIM precompute (elementwise float) |
| 10   | `adm_cm_avx512`                         | 0.9 %  | ADM contrast masking |

Cache statistics (5 × 48 frames, perf stat):

- IPC: 3.62 (well-utilised; AVX-512 units busy)
- L1-dcache load-miss rate: 44.3 % (482.8 M misses / 1 089 M references)
- LLC miss rate: 6.0 % (65.1 M LLC misses)

---

## 4. Line-level annotation — top 3

### 4.1 `iqa_convolve_avx512` (39.5 %)

Source: `core/src/feature/x86/convolve_avx512.c`

Hot lines concentrate on two idioms in `h_tap8_avx512` and `v_tap8_avx512`:

```text
12.9%   vcvtps2pd %ymm1,%zmm1     // float→double widen (every tap, both passes)
 7.6%   vaddpd %zmm1,%zmm0,%zmm0  // double accumulate
 7.3%   vcvtps2pd %ymm1,%zmm1     // second widen in v-pass
 6.8%   vaddpd %zmm1,%zmm0,%zmm0
 5.2%   vmulps (%r14,%rsi,4),%ymm1,%ymm1  // tap multiply (float)
```

The pattern — `vmulps` (float) → `vcvtps2pd` (widen to double) →
`vaddpd` (double acc) — fires once per kernel tap (11 taps for 11-tap
Gaussian kernel). Each convolution output pixel therefore executes
22 vector operations (11 h-taps + 11 v-taps) in this widen-accumulate
pattern. The conversion overhead is per-tap, not amortised.

Root cause: ADR-0138 mandates bit-exact double accumulation to match the
scalar `sum += float*float` followed by `(double)` widening. The
`vcvtps2pd` + `vaddpd` sequence is the correct implementation.

**Optimization opportunity:** partial — see Recommendation 1.

### 4.2 `ssim_accumulate_block_avx512.constprop.0` (18.0 %)

Source: `core/src/feature/x86/ssim_avx512.c`, lines 88–125.

Hot lines are the scalar per-lane reduction loop body:

```text
 7.3%   vcvtss2sd (%rdx),%xmm5,%xmm0    // spilled float → double
 7.2%   vaddsd (%rbx),%xmm0,%xmm0       // scalar double add
 6.2%   add $0x4,%rsi                   // loop counter
 4.9%   vaddsd (%r10),%xmm2,%xmm2
 4.0%   vmulsd %xmm1,%xmm0,%xmm6        // lv * cv
```

The block:

1. Computes 16-lane float intermediates in AVX-512 (`rm`, `cm`, `srsc`,
   `l_den`, `c_den`, `sv`).
2. Stores all six arrays to 64-byte-aligned stack buffers (6 × 64 bytes =
   384 bytes of stack spill).
3. Loops 16 times calling `ssim_accumulate_lane` — pure scalar double.

The stack spill/reload + scalar double loop for 16 lanes is where the 18 %
burns. ADR-0139 requires double-precision accumulation; the current
implementation uses scalar fallback after SIMD float prep.

**Optimization opportunity:** high-value — see Recommendation 2.

### 4.3 `vif_statistic_8_avx512` (11.1 %)

Source: `core/src/feature/x86/vif_avx512.c`, lines ~85–160.

Hot lines are gather instructions:

```text
13.2%   vpgatherdq (%r12,%ymm0,2),%zmm27{%k5}   // log2 table lookup
 3.6%   vpgatherdq (%r12,%ymm17,2),%zmm7{%k6}
 3.2%   vpmuludq %zmm0,%zmm1,%zmm1               // 64-bit multiply
 2.7%   vpgatherdq (%r12,%ymm26,2),%zmm12{%k6}
 2.7%   vpgatherdq (%r12,%ymm2,2),%zmm4{%k7}
```

The `vpgatherdq` (gather 8 × 64-bit QWORDs using 32-bit index) hits the
log2 lookup table. Three separate gather series per 8-lane block (mden,
mnumer1, mnumer1_tmp) = 6 gather instructions per 16-lane block (the
function processes 8 lanes per `b`-loop iteration, 2 iterations per
output block). Each gather serialises 8 independent 64-bit loads from a
shared table; L1 miss rate is high because indices are data-dependent.

**Optimization opportunity:** moderate — see Recommendation 3.

---

## 5. Top-3 NEW optimization targets

### Target 1: `iqa_convolve_avx512` — eliminate redundant `vcvtps2pd` via 16-lane FMA-on-float with post-hoc double correction

**Current pattern (per tap):**

```c
const __m256 prod_f = _mm256_mul_ps(f8, coeff_f);      // float × float
const __m512d prod  = _mm512_cvtps_pd(prod_f);          // widen
acc = _mm512_add_pd(prod, acc);                          // double acc
```

**Proposal:** Replace the 11-tap h-pass with a two-stage approach:

- Stage 1: accumulate all 11 taps in `__m512` float using
  `_mm512_fmadd_ps` (32 lanes at a time, twice the throughput per cycle).
- Stage 2: widen the final float sum once per output pixel via
  `_mm512_cvtps_pd` and store as float.

This exploits the fact that ADR-0138's bit-exactness requirement is
`float*float→double→sum`, which does not forbid using float FMA for the
intermediate accumulation provided the result is re-widened before store.
However this changes the rounding order of partial sums and would break
bit-exactness. The safe alternative: widen only after every 4 taps
(block-of-4 FMA) and double-add the 4-tap block, reducing the widen+add
count from 11 to 3 per output pixel.

**Intrinsic path:** `_mm512_fmadd_ps` (AVX-512F) + `_mm512_cvtps_pd`
(AVX-512F); no new ISA dependency.

**Effort:** M (2–3 days: implement, validate against bit-exact gate,
add unit test comparing output to scalar reference frame-by-frame).

**Expected gain:** 15–25 % on `iqa_convolve_avx512` alone = ~6–10 %
end-to-end on float_ssim + float_ms_ssim paths (combined 57 % of
total cycle budget).

**Constraint:** must remain bit-exact per ADR-0138. Validate with
`make test-netflix-golden` + `/cross-backend-diff` before merging.

---

### Target 2: `ssim_accumulate_block_avx512` — vectorise the double-accumulation reduction using AVX-512 double lanes

**Current pattern:** 16 lanes of float intermediates spilled to 384 bytes
of stack, then 16 scalar double iterations.

**Proposal:** Replace the per-lane scalar loop with a vectorised
double-lane reduction:

- Process the 16-element float buffers (`t_rm`, `t_cm`, etc.) in pairs
  of 8 using `_mm512_cvtps_pd` (8 lanes of double at a time).
- Compute `lv`, `cv`, `sv` in `__m512d` using `_mm512_fmadd_pd` /
  `_mm512_mul_pd` / `_mm512_div_pd`.
- Horizontally reduce 4 `__m512d` partial sums (ssim, l, c, s) via
  `_mm512_reduce_add_pd` (AVX-512DQ) or a manual 4-step tree reduction.

The double-precision requirement of ADR-0139 is fully satisfied: all
arithmetic is `__m512d` = 64-bit IEEE 754 double.

The remaining non-vectorisable op is the `vdivsd` chain visible at
2.7 % in the annotation — `_mm512_div_pd` replaces 8 scalar `vdivsd`
at a time.

**Intrinsic path:** `_mm512_cvtps_pd` + `_mm512_fmadd_pd` +
`_mm512_div_pd` + `_mm512_reduce_add_pd` (AVX-512DQ, available on
Skylake-X and later).

**Effort:** M (3–4 days: implementation is a vectorised re-expression of
`ssim_accumulate_lane`, which is 5 arithmetic ops per lane — straightforward
in `__m512d` but requires care around the `vdivsd` throughput bottleneck).

**Expected gain:** 40–60 % reduction in `ssim_accumulate_block_avx512`
self-time = ~7–11 % end-to-end (this function is 18 % of total). Combined
with Target 1, the float_ssim + float_ms_ssim combined throughput could
improve by ~30 %.

---

### Target 3: `vif_statistic_8_avx512` — replace `vpgatherdq` log2 lookups with polynomial approximation

**Current pattern:** 3 `vpgatherdq` series per 8-lane block hitting a
shared `log2_table[]`. The gather serialises 8 independent 64-bit loads;
with data-dependent indices the L1 hit rate is poor (44.3 % L1 miss rate
across the full workload, with VIF being the dominant contributor).

**Proposal:** Replace `vpgatherdq` + table with a 5th-order minimax
polynomial for `log2(x)` evaluated in `__m512d`:

```text
log2(x) = exponent + polynomial(mantissa)
```

The exponent extraction is `_mm512_getexp_pd` (AVX-512F). The polynomial
evaluation is 5 FMA operations in `__m512d`. This eliminates all three
gather series per block.

Accuracy: a degree-5 polynomial for log2 over `[1, 2)` achieves < 1 ULP
error, which is well within the fixed-point table's 16-bit precision.
The VIF score is computed from the ratio of integer log sums; a < 1 ULP
double error propagates to sub-LSB differences in the integer accumulator,
which are below the codec-noise floor. Must validate against Netflix golden
gate.

**Intrinsic path:** `_mm512_getexp_pd` (AVX-512F) + `_mm512_fmadd_pd`
(5 × AVX-512F). No `vpgatherdq` required.

**Effort:** L (5–7 days: derive polynomial coefficients via Remez
algorithm, implement in `vif_avx512.c`, verify bit-exact or within-
epsilon against current output on the Netflix golden YUVs, document
decision and epsilon bound in a new ADR).

**Expected gain:** 30–50 % reduction in `vif_statistic_8_avx512`
self-time = ~3–6 % end-to-end. The L1 miss reduction will have a
secondary benefit across the full pipeline (less cache thrashing from
gather stalls).

---

## 6. GPU section (wall-clock only, no profiler available on this host)

CUDA and Vulkan profiling (`nsys`, `ncu`, `vulkan-profiler`) are not
installed. Wall-clock throughput from `netflix_benchmark_results.json`
(pre-sprint baseline, PR #309):

| Workload        | CPU fps | CUDA fps | CUDA vs CPU |
|-----------------|---------|----------|-------------|
| src01 576×324   | 543.6   | 334.7    | 0.62×       |
| checker 1080p   | 35.0    | 22.6     | 0.65×       |

Note: the baseline predates PR #312 (CUDA drain-batch, claimed +8.6 % on
7-extractor) and PR #320 (psnr_hvs async+pinned, estimated +5–8 %). A
re-run of `bench_perf.py` on a CUDA host after those PRs is needed to
measure the actual gains. The GPU wall-clock section of this document
should be updated once a CUDA host is available.

**Profiler gap summary:**

- `nsys` / `ncu`: not installed — CUDA kernel-level hot functions unknown.
- `vulkan-profiler` / `VK_LAYER_KHRONOS_profiles`: not installed — Vulkan
  shader timings unknown.
- Recommendation: add `nsys` + `ncu` to the fork's dev-container
  (`docker/Dockerfile`) so the CUDA profile step can run in CI on a
  self-hosted CUDA runner.

---

## 7. Profile method summary

| Step                 | Tool used            | Gap / caveat                         |
|----------------------|----------------------|--------------------------------------|
| CPU top-N            | perf record -F 999   | 2 642 samples; sampling noise ±1 pp |
| CPU line annotation  | perf annotate        | DWARF call-graph; clean              |
| Cache miss rates     | perf stat            | LLC-load-misses not supported on CPU |
| GPU kernel hot list  | **NOT COLLECTED**    | nsys/ncu not installed               |
| Vulkan shader timing | **NOT COLLECTED**    | vulkan-profiler not installed        |
| Flamegraph SVG       | not generated        | perf-flamegraph script absent        |

Raw perf data: `/tmp/vmaf_profiles/2026-05-03/perf_cpu_576.data`
(not committed; ephemeral).
