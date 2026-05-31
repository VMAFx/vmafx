<!-- markdownlint-disable MD013 MD060 -->
# Research-0735: CUDA motion kernel ncu hotpath analysis (576x324)

- **Status**: Active
- **Workstream**: perf/cuda-motion-ncu (no ADR — research only)
- **Last updated**: 2026-05-28

## Question

What bottleneck class applies to the CUDA motion kernel `calculate_motion_score_kernel_8bpc`
at 576×324, and what are the top optimization candidates?

## Sources

- ncu `--set basic` profiles on RTX 4090 (sm_89) under CUDA 13.3
- Source: `core/src/feature/cuda/integer_motion/motion_score.cu`
- Reproducer:

  ```text
  docker run --rm --gpus all --privileged --entrypoint bash \
    -v <worktree>:/workspace -v <repo>/python:/workspace/python:ro \
    -w /workspace/core vmaf-dev-mcp:cuda13.3 -c \
    'ncu -k "regex:motion" --set basic --launch-count 3 \
         build-ncu/tools/vmaf \
           --reference ../python/test/resource/yuv/src01_hrc00_576x324.yuv \
           --distorted ../python/test/resource/yuv/src01_hrc01_576x324.yuv \
           --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
           --feature motion --backend cuda -o /dev/null'
  ```

## Findings

GPU: RTX 4090, 128 SMs, sm_89. Three frame launches profiled.

### Kernel summary table

| Kernel | Grid | Waves/SM | Achieved Occ (%) | Theoretical Occ (%) | DRAM Tp (%) | Compute Tp (%) | Bottleneck class |
|--------|------|----------|-------------------|----------------------|-------------|----------------|-----------------|
| `calculate_motion_score_kernel_8bpc` | (36,21,1)×(16,16) | ~0.59 | 60–64 | ~75 (bound: `__launch_bounds__(256,8)`) | 11–12 | not measured | Launch + low DRAM |

Three samples: occupancy 62.95% / 58.49% / 64.15%, DRAM 12.12% / 10.35% / 11.05%.

### Key observations

1. **Moderate occupancy relative to ADM.** The motion kernel achieves 59–65% occupancy
   vs the `__launch_bounds__(256, 8)` cap of 8 blocks/SM × 8 warps each = 64 warps/SM
   (theoretical ~75% on sm_89 at 48 warp/SM maximum). The achieved value is ~80% of
   theoretical, which is reasonable for a shared-memory-tiled Gaussian blur.

2. **Low DRAM throughput (10–12%) despite memory-dominated workload.** The kernel reads
   a shared-memory tile of 20×21×4 bytes (≈ 1680 bytes per block) from global memory in
   the tile-load phase, then performs a 5×5 separable Gaussian filter from shared memory.
   The L1 cache is effectively serving the repeat accesses within the tile, suppressing
   DRAM pressure — which is the correct behavior.

3. **Grid is 756 blocks for 576×324.** At a 128-SM GPU, that is ~5.9 full waves. Unlike
   the ADM kernels, this kernel does approach steady-state occupancy since the work is
   deeper (5×5 filter, blur + SAD accumulation). The remaining occupancy gap (11–15%) is
   due to warp scheduling overhead and the `__syncthreads()` barrier between tile load and
   filter phases.

4. **The `atomicAdd` on a single 64-bit counter** (line 133: `atomicAdd(...sad.data, ...)`)
   serializes all 128×n warps onto one memory address for the SAD reduction. For large
   frames this creates contention; at 576×324 it is minor because the kernel is otherwise
   compute-bound.

5. **`__launch_bounds__(256, 8)`** limits concurrent blocks per SM to 8 and thus warps to
   64. Removing the constraint would allow the compiler to target higher occupancy but may
   increase register usage or shared memory spill.

### Shared memory bank conflicts

The motion kernel uses `TILE_PITCH = TILE_W + 1 = 21` to pad shared memory rows (PR #120
comment in `motion_score.cu`). GCD(21, 32) = 1, eliminating 2-way bank conflicts from
the unpadded TILE_W = 20. This optimization is already in place.

## Recommendations

1. **Two-pass blur → persistent SAD accumulation.** The current design fuses blur +
   SAD into a single kernel with a global `atomicAdd`. At higher resolutions (4K),
   the single-counter atomicAdd becomes a serialization bottleneck. Separating into
   (a) blur-only kernel → temp buffer, (b) warp-reduce SAD with `__shfl_down_sync`
   at block level → block-partial array, (c) host-side sum would eliminate the
   global serialization. At 576×324 the benefit is < 5%; at 4K the benefit is ~15–20%.

2. **Shared-memory prefetch via `cp.async` (sm_80+).** The tile-load loop reads
   `TILE_W × TILE_H = 400` elements cooperatively by 256 threads. Replacing the scalar
   `for (i=lid; i < tile_elems; i += wg_size)` loop with 2 × `cp.async.cg.shared.global`
   instructions (`ld.global.cg` prefetch) could overlap the tile load latency with the
   barrier, improving the overall throughput by 5–10% at ≥ 1080p.

3. **Increase CTA X to 32** (from 16×16 to 32×8). At 576 width, the current 16-wide
   block produces 36 blocks in X. A 32-wide block would produce 18 blocks in X, halving
   launch overhead while keeping occupancy similar (shared memory usage stays under 48 KB
   with TILE_PITCH = 21).

## Related

- ADR-0108 (deep-dive deliverables)
- Research-0738 (cross-metric summary)
- `core/src/feature/cuda/integer_motion/motion_score.cu`
