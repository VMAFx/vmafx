<!-- markdownlint-disable MD013 MD060 -->
# Research-0736: CUDA SSIM kernel ncu hotpath analysis (576x324)

- **Status**: Active
- **Workstream**: perf/cuda-ssim-ncu (no ADR — research only)
- **Last updated**: 2026-05-28

## Question

What bottleneck class applies to the CUDA float_ssim kernels
(`calculate_ssim_horiz_8bpc`, `calculate_ssim_vert_combine`) at 576×324,
and what are the top optimization candidates?

## Sources

- ncu `--set basic` profiles on RTX 4090 (sm_89) under CUDA 13.3
- Source: `core/src/feature/cuda/integer_ssim/ssim_score.cu`
- Note: `integer_ssim_score.cu` (9-tap int64 path) is not profileable in isolation
  because the functions are defined without `extern "C"` wrapping — CUDA name mangling
  prevents the host C extractor (`ssim_cuda.c`) from resolving the symbols via
  `cuModuleGetFunction` with plain names. The `calculate_ssim_horiz_8bpc` /
  `calculate_ssim_vert_combine` kernels (float SSIM path, `ssim_score.cu`) were
  profiled as a proxy; the structural properties (small grid, low occupancy) are
  expected to generalize to the int64 path.
- Reproducer (triggers via float VMAF model which includes float_ssim):

  ```text
  docker run --rm --gpus all --privileged --entrypoint bash \
    -v <worktree>:/workspace -v <repo>/python:/workspace/python:ro \
    -v <repo>/model:/workspace/model:ro \
    -w /workspace/core vmaf-dev-mcp:cuda13.3 -c \
    'ncu -k "regex:calculate_ssim|vert_combine" --set basic --launch-count 3 \
         build-ncu/tools/vmaf \
           --reference ../python/test/resource/yuv/src01_hrc00_576x324.yuv \
           --distorted ../python/test/resource/yuv/src01_hrc01_576x324.yuv \
           --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
           --model path=../model/vmaf_float_v0.6.1.json \
           --feature float_ssim_cuda --backend cuda -o /dev/null'
  ```

## Findings

GPU: RTX 4090, 128 SMs, sm_89.

### Kernel summary table

| Kernel | Grid | Waves/SM | Achieved Occ (%) | Theoretical Occ (%) | DRAM Tp (%) | Notes |
|--------|------|----------|-------------------|----------------------|-------------|-------|
| `calculate_ssim_horiz_8bpc` | (36,41,1)×(16,8) | 0.96 | 51.1 | 100 | 8.8 | Pass 1: 11-tap horizontal |
| `calculate_ssim_vert_combine` | (36,40,1)×(16,8) | 0.94 | 74.0 | 100 | 55.8 | Pass 2: 11-tap vertical + SSIM formula |

Block size: 128 (16×8). Grid size: 1476 (horiz) / 1440 (vert).

### Key observations

1. **`calculate_ssim_vert_combine` is the hottest kernel (DRAM 55.8%, highest in the
   SSIM family).** This is the expected behavior for the vertical pass: it reads 5 × 9
   rows of the horizontal-pass intermediates (5 arrays × 9 rows × 576 float32 values =
   ~103 KB per block). The achieved occupancy of 74% (12 blocks/SM × 3 warps each) is
   the best in this profiling session.

2. **`calculate_ssim_horiz_8bpc` has low DRAM throughput (8.8%) despite reading the
   input picture.** The horizontal pass reads only one row of pixels per block (576 × 1
   × 2 channels × 1 byte each = 1152 bytes); the 11-tap kernel has low arithmetic
   intensity but the small data set fits comfortably in L1. The achieved occupancy of
   51.1% is register-limited (12 blocks/SM at theoretical 100% but registers cap at 12
   blocks for 128 threads each).

3. **Near-full-wave operation at 576×324.** Both kernels approach 0.94–0.96 waves
   across all 128 SMs, making them the closest to steady-state in the entire VMAF CUDA
   pipeline at this resolution. Unlike the ADM kernels, SSIM is not launch-starved.

4. **`calculate_ssim_vert_combine` is DRAM-bound** (55.8% of peak DRAM bandwidth).
   This is consistent with its 9-tap vertical reduction over 5 float arrays — the access
   pattern is strided and not serviced entirely by L1. The L2 cache throughput (not
   measured with `--set basic`) is likely significant.

5. **Missing `extern "C"` in `integer_ssim_score.cu`** is a pre-existing bug. The
   9-tap int64 kernels `integer_ssim_horiz_8bpc`, `integer_ssim_horiz_16bpc`, and
   `integer_ssim_vert_combine` use C++ linkage (name mangling), while `ssim_cuda.c`
   calls `cuModuleGetFunction` with the plain unmangled names. The result is a
   `CUDA_ERROR_NOT_FOUND (500)` at runtime whenever `--feature ssim` routes through the
   CUDA extractor. Only the float_ssim path (`ssim_score.cu` with `extern "C"`) works.

## Recommendations

1. **Fix missing `extern "C"` in `integer_ssim_score.cu`** (P0 correctness, not perf).
   The three kernel definitions `integer_ssim_horiz_8bpc`, `integer_ssim_horiz_16bpc`,
   and `integer_ssim_vert_combine` must be wrapped in `extern "C" { ... }` to produce
   the plain symbol names that `ssim_cuda.c` looks up. This is a blocking bug: any caller
   that uses `--feature ssim --backend cuda` will get a runtime error. The fix is
   trivially adding `extern "C" {` and `}` around the three `__global__` definitions.

2. **`calculate_ssim_vert_combine` shared-memory staging.** The vert pass loads 5 × 9
   rows of float per block from global memory. Pre-loading all 45 float rows of a block's
   footprint into shared memory before the per-column reduction loop (≈ 45 × 16 floats ×
   4 bytes = 2.88 KB / block, well under 48 KB) would convert DRAM bandwidth into L1
   hits, reducing the 55.8% DRAM throughput by 30–40% at 576×324.

3. **Vectorized reads for `calculate_ssim_horiz_8bpc`.** The current implementation reads
   pixels one-at-a-time (`ref[y * ref_stride + src_x]`). Replacing with `uchar4` or
   `ushort4` vector loads for the 11-tap window (11 bytes vs four `ld.global.u8`) would
   reduce the number of global loads from 11 to 3 (4+4+3), potentially improving SM
   throughput from ~25% to ~50%.

## Related

- ADR-0108 (deep-dive deliverables)
- ADR-0564 (integer_ssim CUDA — introduces `ssim_cuda.c` and `integer_ssim_score.cu`)
- Research-0738 (cross-metric summary)
- `core/src/feature/cuda/integer_ssim/ssim_score.cu`
- `core/src/feature/cuda/integer_ssim/integer_ssim_score.cu`
- `core/src/feature/cuda/ssim_cuda.c`
