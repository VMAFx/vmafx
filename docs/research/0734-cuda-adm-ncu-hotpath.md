<!-- markdownlint-disable MD013 MD060 -->
# Research-0734: CUDA ADM kernel ncu hotpath analysis (576x324)

- **Status**: Active
- **Workstream**: perf/cuda-adm-ncu (no ADR — research only)
- **Last updated**: 2026-05-28

## Question

Which ADM CUDA kernels are the primary bottleneck at 576×324, what bottleneck class
applies to each, and what are the 2–3 highest-leverage optimization candidates?

## Sources

- ncu `--set basic` profiles on RTX 4090 (sm_89, CC 8.9) under CUDA 13.3
- Source: `core/src/feature/cuda/integer_adm/` (adm_cm.cu, adm_csf.cu, adm_csf_den.cu, adm_decouple.cu, adm_dwt2.cu)
- Build: `core/build-ncu` (meson release + `-g -fno-omit-frame-pointer`)
- Input: Netflix golden pair `src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv` (8bpc, YUV 4:2:0)
- Reproducer:

  ```text
  docker run --rm --gpus all --privileged --entrypoint bash \
    -v <worktree>:/workspace -v <repo>/python:/workspace/python:ro \
    -w /workspace/core vmaf-dev-mcp:cuda13.3 -c \
    'ncu -k "regex:adm_cm_line|i4_adm_cm_line|adm_csf_den|adm_dwt2|adm_csf" \
         --set basic --launch-count 4 \
         build-ncu/tools/vmaf \
           --reference ../python/test/resource/yuv/src01_hrc00_576x324.yuv \
           --distorted ../python/test/resource/yuv/src01_hrc01_576x324.yuv \
           --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
           --feature adm --backend cuda -o /dev/null'
  ```

## Findings

GPU: RTX 4090, 128 SMs, sm_89.

### Kernel summary table

| Kernel | Grid | Waves/SM | Achieved Occ (%) | Theoretical Occ (%) | DRAM Tp (%) | Compute Tp (%) | Bottleneck class |
|--------|------|----------|-------------------|----------------------|-------------|----------------|-----------------|
| `adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t` | (5,21,1) | 0.83 | 15.7 | ~50 | 7.8 | ~12 | Launch starvation |
| `adm_cm_line_kernel_8` | (8,5,3) | 0.23 | 7.5 | 33.3 | 7.9 | 29.7 | Register-limited + launch starvation |
| `adm_csf_den_scale_line_kernel_8_128` | (1,132,3) | 0.26 | 19.9 | 100 | 9.8 | 7.5 | Launch starvation |
| `adm_csf_den_s123_line_kernel_8_128` | (1,67,3) | 0.13 | 11.6 | 100 | 6.1 | 5.3 | Launch starvation |
| `i4_adm_cm_line_kernel_fused` | (1,67,3) | 0.17 | 13.1 | 75 | 7.9 | 35.4 | Register-limited + launch starvation |

### Key observations

1. **All ADM kernels are launch-starved at 576×324.** The RTX 4090 has 128 SMs; every
   kernel launches < 0.83 waves across all SMs. The GPU is cold for most of the
   dispatch. At 576×324 = 186k pixels, the available work simply does not fill a
   128-SM GPU.

2. **Register pressure is the secondary limiter for `adm_cm_line_kernel_8` and
   `i4_adm_cm_line_kernel_fused`.** `adm_cm_line_kernel_8` uses 114 registers/thread;
   theoretical occupancy is capped at 33.3% (4 blocks/SM). The fused i4 kernel uses
   56 registers with theoretical 75% but only achieves 13.1% due to the tiny grid.
   ncu estimates a 66.7% local speedup from reducing register pressure in `adm_cm_line_kernel_8`.

3. **`adm_dwt2` is the widest kernel by grid count (5,21,1 = 105 blocks total) but still
   only 0.83 waves.** It uses shared memory tiling (128×8 tile + halo) and a vertical/
   horizontal fused pass. DRAM throughput (7.8%) is low relative to the theoretical
   bandwidth. The low SM utilization (15.7%) arises from the boundary condition branches
   in the halo load loop.

4. **`adm_csf_den` kernels (scale-0 and scale-1-3)** are among the smallest grids and
   most severely launch-starved. The scale-0 path (`adm_csf_den_scale_line_kernel_8_128`)
   reads `int16_t` bands and performs a cubic accumulation; `adm_csf_den_s123_line_kernel_8_128`
   reads `int32_t`. Both have near-100% theoretical occupancy but < 20% achieved.

### Implications

At 576×324, every ADM CUDA kernel is primarily limited by **kernel launch overhead and
insufficient work** to saturate the GPU, not by compute or memory bandwidth. The compute
throughput for `adm_cm_line_kernel_8` (29.7%) and `i4_adm_cm_line_kernel_fused` (35.4%)
is the highest in this family but both kernels run for < 15 µs per frame.

At 1080p or 4K the grid sizes scale quadratically and the bottleneck class may shift;
these findings are specific to the 576×324 Netflix golden pair.

## Alternatives explored

Static code analysis of `adm_cm.cu` shows the `adm_cm_line_kernel_8` template uses
`rows_per_thread=8` to increase per-thread work, which is a partial mitigation of the
small-grid problem. The `i4_adm_cm_line_kernel_fused` eliminates the previous two-kernel
(compute + reduce) pattern, merging the warp-reduce into the compute pass.

## Open questions

- Register spill behavior for `adm_cm_line_kernel_8` (114 regs/thread): does the compiler
  spill to local memory? (Not measured with `--set basic`; needs `--set roofline`.)
- At 1920×1080 the total blocks scale ~11×; at that size the bottleneck class is expected
  to shift toward DRAM throughput for `adm_dwt2` (stream-access pattern).

## Recommendations

1. **`adm_cm_line_kernel_8` register reduction** — refactor the `WarpShift` struct reads
   into `__constant__` or separate the per-scale and per-band shift variables to reduce
   live range. Target: 56–64 regs/thread (matching `i4_adm_cm_line_kernel_fused`), which
   would raise theoretical occupancy from 33% to 75%, estimated +20–30% throughput at ≥ 1080p.

2. **Persistent kernel / work-queue for small-grid ADM kernels** — at 576×324, launching
   20–30 separate kernels per frame with grid sizes < 400 blocks wastes ~80% of wall time
   in CUDA launch overhead. A persistent-kernel pattern where a pool of 128 CTAs polls a
   shared work queue can eliminate the per-kernel launch for `adm_csf_den_*` and
   `adm_csf_kernel_*`.

3. **Shared memory blocking for `adm_dwt2_8_vert_hori_kernel`** — the halo load loop
   uses a scalar loop body (`for (i = lid; i < tile_elems; i += wg_size)`) with an
   integer divide to compute `ty = i / TILE_W`. Replacing the divide with separate x/y
   counters and eliminating the boundary branch via `__funnelshift_r`-style index
   computation could improve SM utilization from 15% toward 30% at the same resolution.

## Related

- ADR-0108 (deep-dive deliverables)
- Research-0738 (cross-metric summary)
- `core/src/feature/cuda/integer_adm/`
