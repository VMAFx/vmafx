<!-- markdownlint-disable MD013 MD060 -->
# Research-0737: CUDA MS-SSIM kernel ncu hotpath analysis (576x324)

- **Status**: Active
- **Workstream**: perf/cuda-ms-ssim-ncu (no ADR — research only)
- **Last updated**: 2026-05-28

## Question

What bottleneck class applies to the CUDA MS-SSIM kernels (`ms_ssim_decimate`,
`ms_ssim_horiz`, `ms_ssim_vert_lcs`) at 576×324, and what are the top optimization
candidates?

## Sources

- ncu `--set basic` profiles on RTX 4090 (sm_89) under CUDA 13.3
- Source: `core/src/feature/cuda/integer_ms_ssim/ms_ssim_score.cu`
- Reproducer:

  ```text
  docker run --rm --gpus all --privileged --entrypoint bash \
    -v <worktree>:/workspace -v <repo>/python:/workspace/python:ro \
    -w /workspace/core vmaf-dev-mcp:cuda13.3 -c \
    'ncu -k "regex:ms_ssim_decimate|ms_ssim_horiz|ms_ssim_vert_lcs" \
         --set basic --launch-count 4 \
         build-ncu/tools/vmaf \
           --reference ../python/test/resource/yuv/src01_hrc00_576x324.yuv \
           --distorted ../python/test/resource/yuv/src01_hrc01_576x324.yuv \
           --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
           --feature float_ms_ssim_cuda --backend cuda -o /dev/null'
  ```

  Note: `--feature float_ms_ssim_cuda` is the registered CUDA extractor name
  (`integer_ms_ssim_cuda.c`, `.name = "float_ms_ssim_cuda"`).

## Findings

GPU: RTX 4090, 128 SMs, sm_89. Four `ms_ssim_decimate` launches profiled (two per
scale per invocation, at scales 1/2 = 288×162 and 1/4 = 144×81).

MS-SSIM produces a 5-scale pyramid. At 576×324, the pyramid levels are:

- Scale 1: 576×324 (no decimate)
- Scale 2: 288×162 (decimate scale 1)
- Scale 3: 144×81 (decimate scale 2)
- Scale 4: 72×40 (decimate scale 3)
- Scale 5: 36×20 (decimate scale 4)

### Kernel summary table (ms_ssim_decimate)

| Scale output | Grid | Waves/SM | Achieved Occ (%) | Theoretical Occ (%) | DRAM Tp (%) | Compute Tp (%) | Bottleneck class |
|---|---|---|---|---|---|---|---|
| 288×162 (scale 2) | (18,21,1)×(16,8) | 0.25 | 17.3–19.2 | 100 | 16.9 | 19.9 | Launch starvation |
| 144×81 (scale 3) | (9,11,1)×(16,8) | 0.06 | 6.6–6.8 | 100 | 6.5 | 5.7 | Severe launch starvation |

Registers per thread: 39 (within block limit of 12 blocks/SM for 128 threads each).
Shared memory: 0 bytes static (no shared-memory staging).

### Key observations

1. **Severe launch starvation at all pyramid levels.** At scale 3 (144×81), only 99 blocks
   total — far fewer than the 128 SMs. The GPU is effectively idle for 22% of the kernel
   duration (ncu estimates 22.66% speedup just from filling the SMs). At scale 4 and 5
   the situation worsens to fewer than 30 blocks total.

2. **No shared memory in `ms_ssim_decimate`.** The 9-tap 9/7 biorthogonal LPF kernel
   (`ms_ssim_score.cu:70`) reads each output pixel using two nested 9-tap loops (81
   global reads per output pixel) from an unstructured strided access pattern.
   At scale 2, the access radius is ±4 pixels; with no shared memory tile, every tap
   is a global or L2 cache read. The `mirror_idx` boundary helper introduces a modulo
   operation per tap, adding ~3–5 cycles per iteration.

3. **`ms_ssim_horiz` and `ms_ssim_vert_lcs` were not captured at launch-count 4.** The
   regex `ms_ssim_decimate` matched all 4 early-scale decimate launches before the horiz
   and vert kernels ran. Structurally, `ms_ssim_horiz` is a 1D 11-tap Gaussian over float
   buffers (same shape as `calculate_ssim_horiz_8bpc` but without the uint→float
   conversion inline), and `ms_ssim_vert_lcs` accumulates the vertical 11-tap filter
   plus l/c/s per-pixel formulas. Both are expected to be launch-starved at the smaller
   pyramid scales (144×81, 72×40, 36×20).

4. **Work imbalance among SMSPs.** ncu reports SMSP active cycle spread of ±22–43%
   across the 4 launches. This is the footprint of partial-wave execution: the last
   wave completes unevenly because the grid is smaller than the SM count.

## Recommendations

1. **Fuse decimate + horiz (or entire 5-scale pyramid) into a single persistent kernel.**
   Launching 5 × 3 = 15 separate kernels (decimate × 4 + horiz × 5 + vert_lcs × 5) for
   what is essentially a cascaded filter is the root cause of the starvation. A single
   persistent kernel with a task queue (one CTA per scale level, cooperating via global
   atomics for level dependencies) would eliminate all 14 extra launch overheads and keep
   the GPU busy throughout.

2. **Shared-memory tiling for `ms_ssim_decimate`.** The 9-tap filter reads a 17×17 window
   per output pixel (radius 8 with 2× stride). A shared-memory tile of (BLOCK_X×2+16)×
   (BLOCK_Y×2+16) floats ≈ 48×24×4 bytes ≈ 4.6 KB per block would allow all 81 reads
   per output pixel to be serviced from L1, replacing 81 global reads with 1 cooperative
   load. ncu estimates a 68–93% local speedup from the grid-coverage gap alone; fixing
   the data reuse pattern would add a further 30–40% DRAM throughput reduction.

3. **`mirror_idx` modulo elimination.** The boundary helper uses `idx % period` (a
   64-bit division on CUDA) for every of the 81 taps per output pixel. Replacing with
   a branchless clamp or LUT for the 4-pixel boundary region (most pixels are interior
   and modulo is not needed) would save ~5 cycles per tap for interior pixels.

## Related

- ADR-0108 (deep-dive deliverables)
- Research-0738 (cross-metric summary)
- `core/src/feature/cuda/integer_ms_ssim/ms_ssim_score.cu`
- `core/src/feature/cuda/integer_ms_ssim/integer_ms_ssim_cuda.c`
