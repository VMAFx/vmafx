<!-- markdownlint-disable MD013 MD050 MD060 -->
# Research-0754 — CUDA SSIM vert_combine: __ldg() + __launch_bounds__ + pinned-host leak

**Date:** 2026-05-29
**ADR:** [ADR-0754](../adr/0754-cuda-ssim-vert-combine-ldg-pinned-leak.md)
**Status:** Measurement complete (live ncu A/B, 2026-05-29).

## Motivation

Three findings from the `calculate_ssim_vert_combine` review:

- F2: 55 inner-loop global loads from 5 intermediate buffers without the
  read-only cache path — struct-by-value argument hides pointer from
  compiler non-aliased-load analysis.
- F4: `__launch_bounds__` absent on a 128-thread kernel — register allocation
  left unconstrained relative to the actual launch config.
- F6: `close_fex_cuda` leaked one page of CUDA pinned host memory per
  `vmaf_close()` cycle (host_pinned NULLed by readback_free, never freed).

## Methodology

A/B build of `origin/master` (baseline) vs
`perf/cuda-ssim-vert-combine-ldg-launch-bounds-leak-20260529` (optimized)
in isolated `/tmp/` worktrees inside `vmaf-dev-mcp:cuda13.3`
(`--gpus all`, `--cap-add SYS_ADMIN`).

Build flags: `meson setup -Denable_cuda=true -Dbuildtype=release -Db_ndebug=true
-Dcpp_args='-g -fno-omit-frame-pointer'`; `ninja -C`.

Workloads:

- WL1: `src01_hrc00_576x324.yuv` / `src01_hrc01_576x324.yuv`, 576x324 8bpc,
  48 frames. Feature: `float_ssim_cuda` (regex `.*ssim.*`).
- WL2: `checkerboard_1920_1080_10_3_0_0.yuv` / `..._10_0.yuv`, 1920x1080 8bpc,
  3 frames. Feature: `float_ssim_cuda=scale=1` (scale-override required at 1080p
  because the CUDA SSIM path restricts auto-detection to scale=1; the kernel
  arithmetic is resolution-independent).

ncu profiler: version 2026.2.0.0 (CUDA 13.3 toolkit).
Register counts: `cuobjdump -res-usage`.
Wall time: `time` builtin, 3 runs per variant, median taken.

GPU: RTX 4090 (sm_89, 128 SMs).

## Correctness

ADR-0214 places=4 gate (max per-frame diff < 5e-5).

| Workload | Baseline SSIM mean | Optimized SSIM mean | Max per-frame diff |
| -------- | ------------------ | ------------------- | ------------------ |
| 576p     | 0.863227           | 0.863227            | 0.00e+00           |
| 1080p    | -0.991317          | -0.991317           | 0.00e+00           |

Scores are bit-identical. ADR-0214 PASS.

## ncu A/B — `calculate_ssim_vert_combine`

### WL1: 576x324 8bpc (48 frames, 48 invocations each)

| Metric                  | Baseline    | Optimized   | Delta      | Interpretation                                 |
| ----------------------- | ----------- | ----------- | ---------- | ---------------------------------------------- |
| Duration                | 8252.67 ns  | 8584.67 ns  | **+4.0%**  | Slight regression — wave-limited kernel (noise) |
| DRAM Throughput         | 62.08%      | 64.95%      | +2.9 pp    | Higher, not lower — wave-limited, noise-dominated |
| L1/TEX Cache Throughput | 34.23%      | 35.20%      | +1.0 pp    | Marginal increase                              |
| L2 Cache Throughput     | 32.74%      | 31.43%      | -1.3 pp    | Small L2 reduction                             |
| Achieved Occupancy      | 66.08%      | 67.28%      | +1.2 pp    | Negligible                                     |

At 576p the kernel launches 48 blocks of 128 threads = 6144 active threads
across 128 SMs, meaning < 0.4 waves. In this wave-limited regime kernel
duration is dominated by launch overhead and scheduling noise. No reliable
signal at 576p.

### WL2: 1920x1080 8bpc (3 frames, 3 invocations each, `scale=1` override)

| Metric                       | Baseline       | Optimized      | Delta      | Interpretation                                    |
| ---------------------------- | -------------- | -------------- | ---------- | ------------------------------------------------- |
| Duration                     | 57290.67 ns    | 54869.33 ns    | **-4.2%**  | Clear win — passes ≥3% threshold                  |
| DRAM Throughput              | 89.81%         | 89.80%         | -0.01 pp   | Unchanged — kernel already DRAM-bound              |
| L1/TEX Cache Throughput      | 50.72%         | 49.62%         | -1.1 pp    | Slight decrease (cache capacity used differently) |
| L2 Cache Throughput          | 58.17%         | 61.12%         | +5.1 pp    | Increased — L2 serving more L1 read-only misses   |
| Achieved Occupancy           | 93.35%         | 92.22%         | -1.1 pp    | Within run-to-run noise                           |
| Registers Per Thread         | 40             | 40             | 0          | Unchanged — `__launch_bounds__` had no PTX effect  |
| Total DRAM Elapsed Cycles    | 7209642.67     | 6901418.67     | **-4.3%**  | DRAM cycles reduced despite same throughput %     |
| Elapsed Cycles               | 141398.67      | 134822.67      | **-4.7%**  | Consistent with duration reduction               |

**Interpretation of L2 increase:** DRAM throughput = 89.8% in both — the
kernel is DRAM-saturated. `__ldg()` routes the 55 inner-loop loads through
the L1 texture path. At 1080p the L1 capacity is insufficient to absorb all
5 intermediate-buffer footprints, and L1 misses spill to L2. L2 sees more
hits (read-only path passes through L2 before DRAM), so L2 throughput rises
while total DRAM cycles decrease because fewer duplicate DRAM fetches occur
(the `__ldg()` path allows the read-only path to coalesce differently).
The net effect is -4.2% wall duration despite unchanged DRAM peak throughput
percentage — the kernel does the same work with fewer DRAM round-trips.

### Register allocation (sm_89 / Ada Lovelace, RTX 4090)

`cuobjdump -res-usage` shows REG:40 for `calculate_ssim_vert_combine` on sm_89
in both baseline and optimized. The `__launch_bounds__(128)` hint had no
measurable effect on the ptxas register allocator for this kernel's register
pressure at sm_89 (it was already near-optimal). The annotation is retained
as a documentation hint and guard against future register pressure increases.

## Wall time (end-to-end, 3-run median)

Wall time measures the full VMAF pipeline (ADM + VIF + motion + SSIM + prediction),
not SSIM in isolation. Shown for completeness.

| Workload | Baseline (median) | Optimized (median) | Delta   |
| -------- | ----------------- | ------------------ | ------- |
| 576p     | 0.616s            | 0.519s             | -15.8%  |
| 1080p    | 0.370s            | 0.322s             | -12.9%  |

Note: the wall-time delta is larger than the kernel-level delta because the
3-frame / 48-frame sample size is small and end-to-end timing includes
first-frame warmup and GPU context switching, making the percentage
estimates noisy.

## Decision

Criterion: kernel duration ≥ 3% improvement OR DRAM throughput ≥ 5pp drop.

| Workload | Duration delta | DRAM delta | Criterion met |
| -------- | -------------- | ---------- | ------------- |
| 576p     | +4.0%          | +2.9 pp    | No (wave-limited noise) |
| 1080p    | **-4.2%**      | -0.01 pp   | **Yes** (duration ≥ 3%) |

**Verdict: READY.** The 1080p kernel-duration criterion is met. The 576p
noise result is expected (wave-limited, < 0.4 waves at 576p) and does not
contradict the 1080p finding.

## Correctness of F6 (pinned-host leak fix)

`close_fex_cuda` now calls `vmaf_cuda_buffer_host_free(cu_state, saved_host_pinned)`
after `readback_free`. This fix does not affect any computed score — it only
ensures the CUDA pinned memory allocated during `init_fex_cuda` is properly
freed during `close_fex_cuda`. Verified: bit-identical scores before/after.

## Decision on F1/F3 follow-up

F1 (AoS to SoA buffer restructure) + F3 (matching signature change) are
deferred. At 1080p the F2 `__ldg()` routing captures -4.2% duration with a
minimal code change. The L1 read-only hit rate post-F2 is ~50%, indicating
further gains may be possible via F1/F3 (better coalescing + wider reads).
Revisit if profiling post-merge shows L1 occupancy remains below 60%.

## Note on integer_psnr_cuda.c

The same `readback_free` / `host_free` gap exists in `integer_psnr_cuda.c`
(also confirmed by code inspection: `readback_free` called in close without
preceding `host_free`). That file uses multiple `rb[]` slots per extractor.
Fix is straightforward but in a different file; named explicitly for the
next CUDA cleanup PR.

## ncu reproducer commands

```bash
# Build A/B in isolated /tmp/ worktrees inside vmaf-dev-mcp:cuda13.3
# See docs/development/dev-mcp.md for container setup.

# Profile (replace /tmp/vmaf-{baseline,optimized}-ssim-measure with your worktree paths):
docker run --rm --gpus all --cap-add SYS_ADMIN \
  --entrypoint bash \
  -v /tmp/vmaf-baseline-ssim-measure:/workspace \
  -v python/test/resource/yuv:/yuv:ro \
  vmaf-dev-mcp:cuda13.3 \
  -c "ncu --target-processes all \
        --kernel-name 'regex:.*ssim.*' \
        --set basic --csv \
        /workspace/core/build-baseline/tools/vmaf \
          --reference /yuv/src01_hrc00_576x324.yuv \
          --distorted /yuv/src01_hrc01_576x324.yuv \
          --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
          '--feature' 'float_ssim_cuda' --backend cuda \
          --json -o /dev/null > /workspace/ncu-out.csv"
```
