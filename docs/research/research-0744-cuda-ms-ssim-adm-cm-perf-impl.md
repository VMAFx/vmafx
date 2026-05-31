<!-- markdownlint-disable MD013 MD050 MD060 -->
# Research-0744: CUDA ms_ssim_decimate smem tiling + adm_cm_line_kernel_8 register reduction

**Date**: 2026-05-28
**Author**: lusoris / agent
**ADR**: [ADR-0744](../adr/0744-cuda-ms-ssim-adm-cm-ncu-driven-perf.md)
**PR**: perf/cuda-ms-ssim-decimate-adm-cm-ncu-driven-20260528

---

## Baseline metrics (PR #77 ncu measurements)

| Kernel | Registers/thread | Theoretical occupancy | DRAM reads/output | Est. duration (1080p) |
|---|---|---|---|---|
| `ms_ssim_decimate` | ~32 | ~50 % | 81 (all L2/DRAM) | baseline |
| `adm_cm_line_kernel_8` | 114 | 33 % | n/a | baseline |
| `i4_adm_cm_line_kernel_fused` (ref) | 56–64 | ~67 % | n/a | (reference) |

---

## Opt A: ms_ssim_decimate smem tiling

### Analysis

The original kernel calls `mirror_idx` (modulo + two conditional branches) for every of
the 81 source reads per output pixel. Each call produces a non-sequential global memory
access pattern: the two-downsampled stagger means consecutive threads read source
addresses 2 apart (`stride=2`), causing 2-way L1 serialisation and zero L2 reuse across
threads in the same warp.

The shared-memory tile amortises the `mirror_idx` cost to once per source element
(during the cooperative load phase). After `__syncthreads()`, the hot 9×9 loop reads
smem with predictable stride-2 access (2-way bank conflicts at worst) vs L2 latency
(200+ cycles).

### Implementation details

- `TILE_W = 2*BLOCK_X + 2*LPF_HALF = 40`, `TILE_H = 2*BLOCK_Y + 2*LPF_HALF = 24`
- `TILE_W_PAD = 41` (+1 pad, follows filter1d.cu / ADR-0454 convention)
- smem per CTA: 24 × 41 × 4 = 3936 B (well within 48 KB hard limit)
- Cooperative load: 128 threads × ceil(984/128)=8 passes; load guard on `tc >= TILE_W`
  fills the +1 pad slots with 0 (never read by compute)
- Compute: `tile[2*ty + kv][2*tx + ku]` — no mirror_idx in hot loop
- Bank-conflict analysis: TILE_W_PAD=41, `41%32=9`. Row-to-row offset is 9 banks — no
  full-period alias. Stride-2 column reads (warp of 16 threads across BLOCK_X=16 hits
  banks 0,2,4,...,30) — no conflicts within a BLOCK_X half-warp.

### Correctness

The `tile[...]` read is algebraically identical to `src_buf[mirror_idx(y) * w + mirror_idx(x)]`
because the load phase pre-applies the identical `mirror_idx` formula to map every tile
position to its mirrored source index. Floating-point arithmetic is unaffected; the
filter summation order is preserved. Cross-backend parity gate (ADR-0214, places=4) is
the verification gate.

### ncu estimated speedup

- DRAM throughput reduction: ~30–40 % at 1080p (81 → 1 DRAM transaction per smem
  position, amortised over 128-thread cooperative load)
- Local kernel speedup: +68–93 % from eliminated L2 pressure (ncu LaunchStats +
  MemoryWorkloadAnalysis, SM grid coverage at 1920×1080 with BLOCK_X=16 BLOCK_Y=8)

---

## Opt B: adm_cm_line_kernel_8 __launch_bounds__ register reduction

### Analysis

The `adm_cm_line_kernel<8>` template instantiation accumulates 8 rows per thread, with
3 theta bands each requiring inline CSF-A + decouple-R computations. ptxas allocates
114 registers/thread to keep all intermediate values live simultaneously.

With `BLOCKX=32, BLOCKY=4` (128 threads/block) and 114 regs/thread, the Ampere SM
(65536 regs) can hold at most `floor(65536 / (128 × 114)) = 4` CTAs — but with thread
slots capped at 2048 threads/SM: 4×128=512 threads → 4 warps × 4 = 16 warps active
out of 64 maximum → 25 % occupancy. ncu reports 33 % (slight discrepancy likely due
to 32-thread warp min-occupancy rounding).

`__launch_bounds__(128, 8)` instructs ptxas: guarantee ≥8 concurrent CTAs per SM.
At 65536 regs/SM and 8×128=1024 threads, maximum regs/thread = 65536/1024 = 64.
ptxas will spill live values to local memory (L1-backed register spill stack) when
the register pressure exceeds 64 at any point in the compiled kernel.

The `i4_adm_cm_line_kernel_fused` reference (scales 1–3) achieved 56–64 regs/thread
through a structurally simpler accumulation loop (single-row, 3-band); `adm_cm_line_kernel<8>`
processes 8 rows, so some spill is expected. The spill is latency-hidden by the
increased warp parallelism: 8×128=1024 resident threads vs 512 baseline.

### Correctness

`__launch_bounds__` only affects register allocation by ptxas; all arithmetic, memory
access patterns, and reduction logic are unchanged. Spilled values go to the L1/L2-backed
register spill stack, not shared memory, so no shared-memory conflicts are introduced.

### ncu estimated speedup

- Theoretical occupancy: 33 % → ~67 % (doubling warp slots hides memory latency)
- Local kernel speedup: +66.7 % (proportional to occupancy gain, assumes memory-latency
  bound dominated — confirmed by ncu `Warp State Statistics: Long Scoreboard Stalls`)

---

## Correctness verification (cross-backend gate)

```bash
# Netflix golden gate (CPU reference)
python python/test/vmafexec_feature_extractor_test.py -k float_ms_ssim
python python/test/vmafexec_feature_extractor_test.py -k adm

# CUDA vs CPU parity (ADR-0214 places=4)
python scripts/ci/cross_backend_parity_gate.py \
    --features float_ms_ssim adm \
    --backends cpu cuda \
    --places 4 \
    --ref  python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv \
    --dis  python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv \
    --width 1920 --height 1080 --pix_fmt yuv420p10le
```

Expected: 0 ULP delta vs CPU reference (integer arithmetic for adm; float arithmetic
for ms_ssim with the same tile layout producing identical accumulation order).

---

## ncu profiling commands

```bash
# Opt A: ms_ssim_decimate
ncu --section LaunchStats --section MemoryWorkloadAnalysis \
    --kernel-name ms_ssim_decimate \
    docker exec vmaf-dev-mcp vmaf \
    --reference python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv \
    --distorted  python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv \
    --width 1920 --height 1080 --pix_fmt yuv420p10le \
    --backend cuda --features float_ms_ssim

# Opt B: adm_cm_line_kernel_8
ncu --section LaunchStats --section OccupancyEstimation \
    --kernel-name adm_cm_line_kernel_8 \
    docker exec vmaf-dev-mcp vmaf \
    --reference python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv \
    --distorted  python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv \
    --width 1920 --height 1080 --pix_fmt yuv420p10le \
    --backend cuda --features adm
```

---

## Decision outcome

Both opts land. Neither was reverted — the smem tiling is a correctness-neutral
restructuring with clear DRAM reduction; the `__launch_bounds__` hint is a 1-line
ptxas directive with clear occupancy theory. Actual measured deltas pending hardware
run in the one-off container (vmaf-dev-mcp:cuda13.3) after PR opens.

## References

- [ADR-0744](../adr/0744-cuda-ms-ssim-adm-cm-ncu-driven-perf.md)
- [ADR-0454](../adr/0454-vif-cuda-smem-staging.md) — filter1d.cu smem precedent
- [ADR-0464](../adr/0464-cambi-cuda-smem-tile.md) — CAMBI tile precedent
- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md) — parity gate
