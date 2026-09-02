# Research Digest 0053 — Post-merge CPU profile, 2026-05-03

**Workstream:** perf / chore
**PR:** `chore/profile-hotpath-2026-05-03`
**Digest type:** investigation (no source changes)
**Related:** `docs/development/post-merge-profile-2026-05-03.md` (full report)

---

## What was investigated

A full CPU perf profile was collected on master tip `981659a3` (post-
sprint merge: PRs #310–#321) to re-baseline the performance landscape
after a sprint that delivered CUDA fence batching (#312), ssimulacra2
host XYB SIMD (#314), Vulkan submit-opt-batch (#319), and psnr_hvs
async+pinned (#320).

**Method:** `perf record -F 999 -g --call-graph dwarf` across 10 × 48
frames of the Netflix `src01_576x324` pair, with `vmaf_bench` and
the canonical `vmaf_v0.6.1.json` model, CPU-only profile build
(`--buildtype=release -Db_ndebug=true -Dc_args="-g -fno-omit-frame-pointer"`).

---

## Key findings

### 1. `iqa_convolve_avx512` dominates at 39.5 %

The separable 2-D Gaussian convolution used by both float_ssim and
float_ms_ssim spends most of its time on `vcvtps2pd` + `vaddpd` — the
ADR-0138-mandated float-multiply → widen → double-accumulate pattern,
once per kernel tap. With an 11-tap kernel this fires 22 times per
output pixel. The current implementation issues one `vcvtps2pd` per
tap; blocking 4 taps before widening would reduce conversions by 3.7×
without breaking bit-exactness.

### 2. `ssim_accumulate_block_avx512` costs 18.0 % — scalar double loop is the culprit

The function correctly uses AVX-512 float for the six intermediates but
then spills them to 384 bytes of stack and loops 16 times in scalar
double to honour ADR-0139's double-precision accumulation contract. The
spill-reload alone accounts for roughly half the stall budget. Replacing
the 16-iteration scalar loop with an `__m512d` vectorised reduction (two
8-wide passes) would collapse this to ~2 vector divisions and 4 FMA
chains.

### 3. `vif_statistic_8_avx512` costs 11.1 % — gather stalls on log2 table

Three independent `vpgatherdq` series per 8-lane block hit a shared
`log2_table[]` with data-dependent indices. L1-dcache load-miss rate
across the full workload is 44.3 %. Replacing the table lookups with a
5th-order minimax polynomial for log2 (using `_mm512_getexp_pd` +
`_mm512_fmadd_pd`) would eliminate all three gather series and reduce
pressure on the L1D cache.

### 4. CPU throughput improved +17.6 % vs pre-sprint baseline

Full-pipeline vmaf_v0.6.1 at 576×324: 650.7 fps best vs 553.1 fps in
`netflix_benchmark_results.json` (PR #309). No regression. The gain
is attributable primarily to PRs #312 and incremental scheduling
improvements.

### 5. GPU profiles not collected — tooling gap

`nsys`, `ncu`, and `vulkan-profiler` are absent from the dev workstation.
The CUDA and Vulkan sections remain unquantified. Adding these tools to
`docker/Dockerfile` (or the self-hosted runner image) is the blocking
prerequisite for GPU hot-function analysis.

---

## Dead ends / rejected approaches

- **FP16 accumulation in convolution:** would halve the widen/add cost but
  breaks the ADR-0138 bit-exactness invariant entirely. Rejected.
- **Software prefetch before gather:** `_mm512_prefetch_i32gather_pd` is
  AVX-512PF (Xeon Phi only); not available on desktop Skylake-X / Alder
  Lake. Rejected.
- **SVML `_mm512_log2_pd`:** vendor-specific; not in open-source toolchains;
  accuracy/reproducibility undefined. Rejected in favour of minimax polynomial.

---

## Prior art

- ADR-0138: iqa_convolve AVX2/AVX-512 bit-exact double constraint.
- ADR-0139: SSIM accumulation double-precision contract.
- ADR-0125: ms_ssim_decimate SIMD design.
- `docs/development/coverage-gap-analysis-2026-05-02.md`: sister
  investigation (coverage, not performance).
