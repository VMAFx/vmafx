<!-- markdownlint-disable MD013 MD060 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
# Research-0734: CUDA VIF filter1d ncu Hotpath Profile (2026-05-28)

## Summary

Nsight Compute (ncu 2026.2.0.0) profile of all `filter1d_*` kernels in
`core/src/feature/cuda/integer_vif/filter1d.cu` on an NVIDIA RTX 4090
(sm_89), driver R610.43.02, CUDA toolkit 13.3. Workload: Netflix golden
pair `src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv`, 8-bit 420p,
model `vmaf_v0.6.1.json`, 48 frames.

Raw report artifact: `ncu-filter1d.ncu-rep` (not committed; excluded via
`.gitignore` — too large for tree).

---

## Workload and environment

| Item | Value |
|---|---|
| GPU | NVIDIA RTX 4090 (sm_89, 128 SMs, 16 384 cores) |
| Driver | R610.43.02 |
| CUDA Toolkit | 13.3 (ncu 2026.2.0.0, build 37790515) |
| Build | `meson setup core/build-ncu core -Denable_cuda=true --buildtype=release -Dc_args='-g -fno-omit-frame-pointer' -Dcpp_args='-g -fno-omit-frame-pointer'` |
| Commit | `61ff5e0565` (`research/cuda-vif-ncu-hotpath-20260528`) |
| Workload | 576×324 8-bit 420p, 48 frames, `vmaf_v0.6.1.json`, `--backend cuda` |
| ncu set | `--set basic` |

---

## Kernel inventory and relative hotspot

8 distinct `filter1d_*` kernel variants were captured (4 VIF pyramid
scales × 2 passes each). 48 frames × 8 kernels/frame = 384 kernel
launches total. Duration breakdown:

| Rank | Kernel | Total (us) | Avg (us) | Share |
|---|---|---|---|---|
| 1 | `filter1d_8_horizontal_kernel_2_17_9` | 998.8 | 20.8 | **35.3 %** |
| 2 | `filter1d_16_horizontal_kernel_2_9_5_1` | 381.8 | 7.9 | 13.5 % |
| 3 | `filter1d_8_vertical_kernel_uint32_t_17_9` | 355.7 | 7.4 | 12.6 % |
| 4 | `filter1d_16_horizontal_kernel_2_5_3_2` | 260.0 | 5.4 | 9.2 % |
| 5 | `filter1d_16_vertical_kernel_uint2_9_5_1` | 231.5 | 4.8 | 8.2 % |
| 6 | `filter1d_16_horizontal_kernel_2_3_0_3` | 217.0 | 4.5 | 7.7 % |
| 7 | `filter1d_16_vertical_kernel_uint2_5_3_2` | 199.2 | 4.1 | 7.0 % |
| 8 | `filter1d_16_vertical_kernel_uint2_3_0_3` | 183.1 | 3.8 | 6.5 % |
| **Total** | | **2 827.2** | | |

The scale-0 8-bit horizontal pass (`filter1d_8_horizontal_kernel_2_17_9`)
accounts for 35 % of all VIF filter time. The scale-0 pass processes the
full-resolution frame (576×324) with the widest filter (17 taps, 7
channels) and is the focus of this profile.

---

## Top-3 diagnostic metrics (scale-0 horizontal kernel)

### Metric 1 — Achieved occupancy vs theoretical: 48.6 % / 75 %

- Block: `(128, 1, 1)`, grid: `(3, 324, 1)` — 972 blocks total.
- Theoretical occupancy ceiling: 75 % (limited by **56 registers/thread**;
  register file caps at 64 warps/SM at 128 threads/block ≡ 4 warps ×
  max 9 blocks = 36 warps/SM theoretical).
- Achieved occupancy: **48.6 %** (ncu rule `AchievedOccupancy` estimates
  35 % estimated speedup from closing this gap).
- Root cause: **only 0.84 full waves** across 128 SMs for this 576-wide
  input. The grid of 972 blocks divides to 7.6 blocks/SM; with 4 warps
  per block at 75 % theoretical occupancy each SM sustains ~28 warps when
  occupied, but the tail partial wave leaves many SMs idle.

### Metric 2 — DRAM throughput: 28.7 % of peak

- DRAM Throughput: **28.7 %** of peak sustained.
- Memory Throughput: **28.7 %** (DRAM-bound, not L1-bound).
- L1/TEX: 27.1 %, L2: 11.0 %.
- The smem staging already tiles 7 `uint32_t` channels × TILE_W=273
  elements = 7 644 B per block. The high DRAM throughput despite smem
  staging indicates that the smem load phase still issues 7 separate
  global loads per smem element (one per channel) with 128-thread
  coalescing. The ncu `WorkloadImbalance` rule flags L2 imbalance of
  up to **+46 %** above average across L2 slices — characteristic of
  non-power-of-two stride patterns causing slice hot-spotting.

### Metric 3 — Compute (SM) throughput: 25.4 % of peak

- Compute throughput: **25.4 %**, well below the 28.7 % DRAM throughput.
- Both numbers are low; neither is saturated.
- Primary diagnosis: **latency-bound / launch-width-limited** — the GPU
  is not compute-bound, not fully memory-bound, but is achieving low
  utilisation on both pipelines simultaneously due to insufficient waves
  to hide stall latency and an imbalanced L2 workload.

---

## Diagnosis

**Primary bottleneck: launch-width / wave-count starvation** (0.84 waves)
amplified by **register pressure** (56 regs/thread → 75 % theoretical
occupancy ceiling) and **L2 imbalance** (up to +46 % hot-slice skew).

This is characteristic of kernels that were tuned for low-latency per-
frame execution at smaller resolutions, rather than for throughput on a
128-SM Ada Lovelace die. At 576 × 324 with 128-thread blocks, the grid
fills less than one SM wave; the GPU spends a material fraction of the
kernel window with half its SMs idle. Shared memory staging (already
in place from the 2026-05-16 win #1 / win #4 pass) has reduced DRAM
traffic, but occupancy remains the limiting factor.

No warp divergence signature was detected from the available `--set basic`
metrics (no Warp State Statistics section). The interior/border branch in
the horizontal kernel is expected to produce minimal divergence because
border blocks represent at most 2 of 3 columns at 576-wide.

---

## Optimization candidates

### Candidate 1 — Increase `val_per_thread` from 2 to 4 in the 8-bit horizontal kernel

**What**: Change the `FILTER1D_8_HORI(2, 17, 9)` instantiation to
`FILTER1D_8_HORI(4, 17, 9)`. The horizontal kernel processes
`BLOCKX × val_per_thread = 128 × 2 = 256` output pixels per block.
Doubling `val_per_thread` to 4 doubles output pixels per block
(512 per block) and halves grid width (`ceil(576/512)=2` x-blocks vs 3),
halving total blocks from 972 to 648. This reduces L2 hot-spot imbalance
(fewer scattered small loads) and allows the integer accumulators in the
symmetric-filter loop to reduce register pressure via strength-reduction.
The TILE_W grows to `128×4+16+1=529` u32 (7 channels × 4 B × 529 = 14 812 B
per block — below the 48 KB shared memory limit per block on Ada).

**Expected impact**: 10–20 % reduction in kernel duration for the 8-bit
horizontal pass (i.e. ~3–7 % of total VIF filter time). ncu reports
35 % estimated speedup from the occupancy gap; half of that gap is wave-
count, addressable by doubling pixels/block.

### Candidate 2 — Reduce register pressure via local accumulator refactor

**What**: The horizontal kernel carries 14 register-resident accumulator
arrays across the filter loop: `accum_mu1[2]`, `accum_mu2[2]`,
`accum_ref[2]`, `accum_dis[2]`, `accum_ref_dis[2]`,
`accum_ref_tmp[2]` (uint64), `accum_dis_tmp[2]` (uint64),
`accum_ref_dis_tmp[2]` (uint64), `accum_ref_rd[1]`, `accum_dis_rd[1]`
plus loop variables — consistent with the measured 56 registers/thread.
Splitting into two serialised sub-loops (first compute mu1/mu2, write to
smem or per-thread stack, then reread for the statistics pass) can halve
the live register window at the cost of an extra `__syncthreads()`.
On 128-SM Ada with only 0.84 waves, the extra sync cost is negligible
compared to occupancy gain.

**Expected impact**: If registers/thread drops from 56 to ≤48, theoretical
occupancy improves from 75 % to 83.3 % (matching the vertical kernel).
ncu's `TheoreticalOccupancy` rule estimates **25 % local speedup** from
this register-ceiling lift alone.

### Candidate 3 — Use `__ldg()` read-only loads for smem load phase (7 channels)

**What**: The smem load phase in `filter1d_8_horizontal_kernel` reads
`buf.tmp.mu1`, `.mu2`, `.ref`, `.dis`, `.ref_dis`, `.ref_convol`,
`.dis_convol` from global memory. These 7 arrays are written once (by
the vertical pass) and only read in the horizontal pass — a classic
read-only producer-consumer pattern. Qualifying the pointers as
`const __restrict__` and accessing via `__ldg()` (or `__builtin_nv_ldg`)
routes loads through the read-only L1 path on Ada, which does not
conflict with the shared-memory bank, and enables the texture cache to
prefetch the next tile while the current tile is being computed.
The L2 hot-slice imbalance (up to +46 %) is a secondary symptom that
`__ldg()` helps by distributing across the texture unit's independent
cache lines.

**Expected impact**: 5–15 % reduction in smem load latency (ncu L2
imbalance rule reports +46 % hot-slice; flattening that imbalance yields
the stated 20 % workload imbalance estimated speedup at the L2 level,
translating to ~5 % end-to-end kernel improvement given DRAM is 28.7 %
of peak).

---

## Regression check

No prior `testdata/perf_benchmark_results.json` entry covers the 576×324
CUDA VIF filter-only benchmark. Baseline for future comparison:

- `filter1d_8_horizontal_kernel_2_17_9`: avg 20.8 µs / frame (576×324 8-bit)
- Total VIF filter1d wall time (48 frames): 2 827 µs

---

## Reproducer

```sh
WORKTREE=/home/kilian/dev/vmaf/.claude/worktrees/agent-a9e0e7a395c4d31ae
YUVSRC=/home/kilian/dev/vmaf/python/test/resource/yuv
MODELS=/home/kilian/dev/vmaf/model

# Step 1 — build (inside core/ so nvcc -I ../src resolves correctly)
docker run --rm --gpus all --entrypoint="" \
  -v "$WORKTREE":/workspace -w /workspace \
  vmaf-dev-mcp:cuda13.3 \
  bash -c 'meson setup core/build-ncu core -Denable_cuda=true -Denable_sycl=false \
    --buildtype=release -Dcpp_args="-g -fno-omit-frame-pointer" \
    -Dc_args="-g -fno-omit-frame-pointer" && ninja -C core/build-ncu'

# Step 2 — profile (--privileged required for RmProfilingAdminOnly=1)
docker run --rm --gpus all --privileged --entrypoint="" \
  -v "$WORKTREE":/workspace \
  -v "$YUVSRC":/yuv:ro \
  -v "$MODELS":/models:ro \
  -w /workspace \
  vmaf-dev-mcp:cuda13.3 \
  ncu --target-processes all --kernel-name-base function \
      -k "regex:filter1d" --set basic \
      --export /workspace/ncu-filter1d \
      /workspace/core/build-ncu/tools/vmaf \
        -r /yuv/src01_hrc00_576x324.yuv \
        -d /yuv/src01_hrc01_576x324.yuv \
        -w 576 -h 324 -p 420 -b 8 \
        -m path=/models/vmaf_v0.6.1.json \
        --backend cuda -o /tmp/vif-scores.json --json

# Step 3 — export CSV for analysis
docker run --rm --gpus all --entrypoint="" \
  -v "$WORKTREE":/workspace -w /workspace \
  vmaf-dev-mcp:cuda13.3 \
  ncu --import /workspace/ncu-filter1d.ncu-rep --csv --page details \
  > /tmp/ncu-metrics-full.csv
```

Report artifact: `ncu-filter1d.ncu-rep` in worktree root (excluded from
git by `*.ncu-rep` pattern in `.gitignore`).
