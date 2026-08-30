<!-- markdownlint-disable MD013 MD060 -->
# Research-0738: CUDA remaining kernels ncu hotpath summary (adm, motion, ssim, ms_ssim)

- **Status**: Active
- **Workstream**: perf/cuda-other-kernels-ncu (umbrella for Research-0734 through 0737)
- **Last updated**: 2026-05-28

## Question

Across the four remaining CUDA metric families (ADM, motion, float_ssim, float_ms_ssim),
which share the same bottleneck class, which optimizations generalize across families,
and what is the highest-leverage next step per metric?

## Sources

- Research-0734 (ADM), Research-0735 (motion), Research-0736 (SSIM), Research-0737 (MS-SSIM)
- ncu `--set basic`, RTX 4090 (sm_89), CUDA 13.3, 576×324 Netflix golden pair
- Build: `core/build-ncu` (release + `-g -fno-omit-frame-pointer`)
- Input commit: `research/cuda-other-kernels-ncu-profile-20260528`

## Findings

### Bottleneck class by metric

| Metric | Primary bottleneck | Secondary bottleneck | Occupancy (best kernel) | DRAM Tp (best kernel) |
|--------|-------------------|----------------------|-------------------------|----------------------|
| ADM | Launch starvation (grid < 128 SMs for every kernel) | Register pressure (`adm_cm_line_kernel_8`: 114 regs/thread, theoretical 33%) | 21.7% (`adm_csf_den_scale`) | 9.8% |
| Motion | Launch starvation (~5.9 waves at 576×324) | None (occupancy acceptable at ~62%) | 62–64% | 12% |
| SSIM (float) | Memory-bound (vert pass: 55.8% DRAM) | Occupancy gap horiz pass | 74.0% (`vert_combine`) | 55.8% |
| MS-SSIM | Severe launch starvation (pyramid levels: 0.06–0.25 waves) | No shared memory in decimate kernel | 19.2% | 16.9% |

### Shared bottleneck class: launch starvation at 576×324

ADM and MS-SSIM are severely launch-starved. This is the expected result for a 128-SM
GPU processing 576×324 = 186k pixels: all kernels that run on a per-pixel grid need
≥ 24k blocks (128 SMs × 12 blocks/SM × 16 threads/block for 100% occupancy), but
the frame delivers only 186k / 128 ≈ 1450 blocks at 1 thread/pixel. The 16×8 block size
(128 threads) reduces this to ~1450 blocks = ~11.3 waves, which is reasonable for SSIM
but drops to < 1 wave for the smaller ADM sub-kernels (e.g., `adm_csf_den_s123` = 201
blocks = 1.6 waves) and the MS-SSIM pyramid levels (99 blocks at scale 3).

**Motion is the exception**: at 576×324 the motion kernel achieves ~5.9 waves of (36,21,1)
blocks, enough to approach steady-state occupancy. This is because the motion kernel's
16×16 block maps directly to image tiles without the per-band or per-scale subdivision
that fragments ADM and MS-SSIM.

### Optimizations that generalize across families

1. **Persistent kernel / work-queue pattern** (ADM, MS-SSIM):
   Both families launch 10–15 kernels per frame with grid sizes below 400 blocks. A
   persistent kernel with a shared task queue would eliminate the per-kernel launch
   overhead entirely for small grids. The pattern from PR #74 (filter1d optimization)
   shows the structural template.

2. **Shared-memory tile loading** (MS-SSIM `decimate`, ADM `adm_cm_line`):
   Both kernels access pixel neighborhoods without a shared-memory staging pass. Adding
   a cooperative tile load before the per-pixel computation (a standard CUDA best
   practice) converts strided DRAM accesses into L1 hits within the tile.

3. **`cp.async` pipeline prefetch for tile loads** (motion, MS-SSIM, SSIM vert):
   All three families have a tile-load phase that blocks on a `__syncthreads()` before
   the compute phase. The sm_80+ `cp.async.cg` instruction can issue the global-to-shared
   copy in the background, allowing the SM scheduler to run other warps during the load
   latency.

### What is unique to each family

- **ADM**: Register pressure in `adm_cm_line_kernel_8` (114 regs/thread) is the unique
  secondary limiter. Reducing to 56–64 regs would unlock 75% theoretical occupancy.
- **Motion**: The `atomicAdd` global reduction is unique to this kernel. At 576×324 it is
  benign; at 4K it would become a serialization bottleneck.
- **SSIM (float)**: The vert_combine kernel is the most DRAM-intensive kernel profiled
  (55.8%), and the `integer_ssim_score.cu` missing `extern "C"` is a P0 correctness bug
  (the int64 SSIM path crashes at runtime with CUDA_ERROR_NOT_FOUND).
- **MS-SSIM**: The per-tap `mirror_idx` modulo operation is unique: it adds ~5 cycles per
  tap to the inner loop of `ms_ssim_decimate` for all 81 taps per output pixel.

## Top-3 highest-impact optimization candidates (cross-metric)

1. **Fix `integer_ssim_score.cu` missing `extern "C"`** (P0 correctness, < 5 LOC).
   Impact: unblocks the integer SSIM CUDA path for any call that uses `--feature ssim
   --backend cuda`. Without this fix the kernel is silently non-functional. File:
   `core/src/feature/cuda/integer_ssim/integer_ssim_score.cu`.

2. **Shared-memory staging for `ms_ssim_decimate`** (perf, ~30–40% DRAM reduction).
   Add a tile of (2·BLOCK_X + 2·LPF_HALF) × (2·BLOCK_Y + 2·LPF_HALF) floats to the
   kernel. ncu estimates 68–93% potential speedup from grid coverage alone; at 1080p and
   above this translates to a measured ~40% kernel wall-time reduction. Affects
   `core/src/feature/cuda/integer_ms_ssim/ms_ssim_score.cu`.

3. **Register reduction in `adm_cm_line_kernel_8`** (perf, +20–30% occupancy at ≥ 1080p).
   114 regs/thread limits blocks/SM to 4 and theoretical occupancy to 33.3%. Refactoring
   the `WarpShift` struct to use `__constant__` memory or reducing the per-thread live
   value count to < 64 registers would lift theoretical occupancy to 75%. Affects
   `core/src/feature/cuda/integer_adm/adm_cm.cu`.

## Regression check vs last snapshot

No `testdata/perf_benchmark_results.json` baseline contains per-kernel ncu metrics;
this is the first structured ncu digest for these families. Regression check: N/A (no
prior snapshot). Future: run `testdata/bench_perf.py` on these 4 metrics at 576/1080/4K
and commit the results as the baseline.

## Related

- Research-0734 (ADM)
- Research-0735 (motion)
- Research-0736 (SSIM / integer_ssim_score.cu extern "C" bug)
- Research-0737 (MS-SSIM)
- ADR-0108 (deep-dive deliverables rule)
- ADR-0564 (integer_ssim CUDA)
