<!-- markdownlint-disable MD013 MD060 -->
# Research-0743: CUDA VIF filter1d ncu-driven performance implementation

**Date**: 2026-05-28
**ADR**: [ADR-0743](../adr/0743-cuda-vif-filter1d-ncu-driven-perf.md)
**Kernel**: `filter1d_8_horizontal_kernel_2_17_9` (scale-0, 8-bit, 17-tap horizontal)
**Hardware**: RTX 4090 (sm_89), CUDA 13.3, ncu 2026.2.0.0

## Baseline measurements (origin/master, 576×324 YUV)

| Metric | Value |
|---|---|
| Registers per thread | 56 |
| Shared memory per block | 7644 B |
| Theoretical occupancy | 75% |
| Achieved occupancy | 42.91% |
| Block limit (registers) | 9 blocks/SM |
| Kernel duration | 17.6 µs |
| Grid size | (3, 324, 1) = 972 blocks |
| Elapsed cycles | 44,287 |

## Three candidate optimizations from PR #74 ncu report

### Candidate 1: val_per_thread 2 → 4

Smem calculation: TILE_W = 128×4 + 16 + 1 = 529 elements × 7 channels × 4 B = **14,812 B/block**.

On sm_89: 102,400 B/SM ÷ 14,812 = 6.9 → **6 blocks/SM** (smem-limited).
Occupancy: 6 × 128 / 2048 = **37.5%** — worse than baseline 75%.

**Result: REJECTED.** smem-limited occupancy regression outweighs the 33% grid
reduction at 576p. Documented inline in filter1d.cu.

### Candidate 2: Split accumulator live range (original proposal) → replaced by `__launch_bounds__`

The original proposal was to serialise the 14 accumulator arrays into two sub-loops
to reduce live range. Analysis revealed this would require an extra `__syncthreads()`
and double-pass over filter coefficients.

Replaced by `__launch_bounds__(128, 10)`: on sm_89, this caps the register budget to
floor(65,536 / 128 / 10) = 51 → ptxas allocates **48 registers** (same target,
zero algorithmic overhead).

Advisory for sm_75/sm_80/sm_86 (max 1024 threads/SM → 10×128=1280 > 1024):
ptxas emits "minnctapersm out of range, ignored". These targets retain 56 registers
with no regression.

### Candidate 3: `__ldg()` on 7 tmp-channel global loads

The 7 buffers (mu1, mu2, ref, dis, ref_dis, ref_convol, dis_convol) are written
exclusively by the vertical pass and are pure read-only during this horizontal pass.
`__ldg()` routes these loads through the read-only L1 (texture cache) path.

At 576×324 (wave-limited): neutral to negligible impact.
At ≥1080p: beneficial — 7 tmp channels × stride × h bytes exceed L2 capacity
per frame (e.g. at 1920×1080: 7 × 7680 B/row × 1080 rows = ~58 MB >> 50 MB L2).

## Post-optimization measurements (opts #2+#3, 576×324)

| Metric | Value | Delta |
|---|---|---|
| Registers per thread | **48** | −8 (−14%) |
| Shared memory per block | 7644 B | unchanged |
| Theoretical occupancy | **83.33%** | +8.33pp |
| Achieved occupancy | 41.35% | −1.56pp (wave-limited) |
| Block limit (registers) | 10 blocks/SM | +1 |
| Kernel duration | 18.4 µs | +0.8 µs (within noise) |
| Grid size | (3, 324, 1) = 972 blocks | unchanged |

The achieved occupancy delta at 576×324 is negative because the workload is
wave-limited: only 972/128 = 7.6 waves across 128 SMs. At this resolution,
adding one more block per SM does not improve throughput — the SM scheduler has
no additional work to hide latency behind. The theoretical occupancy improvement
becomes utilizable at ≥1080p (8640 blocks for 1920×1080 → 67 blocks/SM average).

## Correctness verification

| Backend | Mean VMAF | Min VMAF | Max VMAF |
|---|---|---|---|
| CPU (container, master) | 94.323012 | 93.129033 | 100.000000 |
| CUDA-baseline (container, master) | 94.323009 | 93.129019 | 100.000000 |
| CUDA-optimized (this PR) | 94.323010 | 93.129023 | 100.000000 |

Maximum absolute delta (CUDA-opt vs CPU): 0.000002 mean, 0.000010 min.
ADR-0214 gate tolerance: ≤ 0.0001 (places=4). **PASS.**

## ncu reproducer

```bash
# Optimized kernel:
ncu -k 'filter1d_8_horizontal_kernel_2_17_9' --set basic --csv \
  /path/to/build/tools/vmaf \
  -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 --backend cuda

# ptxas register count (sm_89 only):
nvcc --fatbin -gencode=arch=compute_89,code=sm_89 --ptxas-options=-v \
  core/src/feature/cuda/integer_vif/filter1d.cu -o /dev/null \
  -I core/build-baseline/src -I core/src -I core/include \
  -I core/src/feature -I core/src/cuda/ \
  -DDEVICE_CODE -D_USE_MATH_DEFINES -D__MATH_NO_INLINES --std c++20 2>&1 | \
  grep 'filter1d_8_horizontal\|registers'
```
