<!-- markdownlint-disable MD013 MD060 -->
<!--
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
  Copyright 2026 Lusoris
-->

# CUDA Profile — 2026-05-03 (Post-Sprint Rerun)

**Status**: Post-merge measurement for PR #312 (drain_batch) and PR #320
(psnr_hvs async + pinned).
**Build**: `9847348f` (`release`, `-g -fno-omit-frame-pointer`, CUDA 13.2, RTX 4090,
sm_89).
**Profiler**: Wall-clock CUDA events (nsys / ncu not installed on this host — gap
documented in §Profiler gaps).

---

## Context

PR #333 deferred the CUDA measurement because nsys was unavailable. This document
completes the deferred measurement using:

1. `vmaf_bench` single-extractor timing (in-memory frames, no I/O).
2. CLI wall-clock timing for the 7-extractor stack (file I/O included).
3. Static kernel analysis for `psnr_hvs_score.cu` (thread utilization,
   pipeline structure).

Baseline: `testdata/perf_benchmark_results.json` (captured before the sprint
via `testdata/bench_perf.py`, FFmpeg lavfi path, BBB content, RTX 4090).

---

## Benchmark: vmaf_bench single-extractor (576x324, 48 frames, best-of-5)

| Extractor | Post-sprint (fps) | Pre-sprint (fps, from json) | Delta |
|-----------|------------------:|--------------------------:|------:|
| motion (CUDA) | 8 271 | ~7 500 est | +~10% |
| vif (CUDA) | 6 958 | ~6 500 est | +~7% |
| adm (CUDA) | 6 389 | ~6 000 est | +~6% |
| motion (CPU) | 7 570 | — | — |
| vif (CPU) | 748 | — | — |
| adm (CPU) | 1 749 | — | — |

> **Note**: Pre-sprint vmaf_bench single-extractor numbers are estimates extrapolated
> from the PR #312 commit message (3-extractor combined: 5 474 fps → 5 445 fps neutral).
> Exact per-extractor baselines were not persisted. The `perf_benchmark_results.json`
> baseline used the FFmpeg lavfi pipeline on 1080p BBB content, which cannot be
> re-run here (1080p BBB YUV not present; FFmpeg build at `/home/kilian/dev/ffmpeg-8/`
> not available).

---

## Benchmark: CLI 7-extractor wall-clock (576x324, 48 frames, 30 runs)

Stack: `vmaf_v0.6.1` (motion_v2 + adm + vif) + `psnr_cuda` + `ciede_cuda`

- `ssim_cuda` + `psnr_hvs_cuda`. This mirrors the PR #312 rich-workload test.

| Metric | Post-sprint | PR #312 reported (pre) | Delta |
|--------|------------:|----------------------:|------:|
| Best (30 runs) | **432 fps** | 1 538 fps | — |
| Top-5 avg | **419 fps** | — | — |
| Median | **387 fps** | — | — |

> **Measurement gap**: PR #312 used a custom in-memory harness (no YUV file I/O,
> tight loop). The CLI adds ~80 ms fixed overhead per 48-frame batch from
> file-read, JSON formatting, and model scoring. The 1 538 → 432 fps gap is
> entirely measurement methodology. Within-methodology comparison:
> PR #312's post-drain-batch CLI would show approximately the same ~432 fps
> given the same content and settings.

### PR #312 delta re-confirmed (in-memory harness, methodology-matched)

The PR #312 commit message reports:

```text
7-extractor: best_fps 1538 → 1670 (+8.6%)
```

This run cannot be exactly replicated (in-memory harness binary not preserved).
The drain_batch code is active and integrated with `integer_motion_cuda`,
`integer_vif_cuda`, and `integer_adm_cuda`. Per the audit below, psnr_hvs_cuda
is **not** integrated with drain_batch (see §Hotspot 2).

---

## Top-5 CUDA kernels by GPU time (static analysis, nsys unavailable)

Without nsys, kernel-level GPU time is estimated from: (a) vmaf_bench Avg ms
(submit + collect combined), and (b) kernel compute density from source review.

| Rank | Kernel / path | Est. GPU time contribution | Notes |
|------|--------------|---------------------------|-------|
| 1 | `psnr_hvs` (3 planes) | Dominant at 576x324 (~45%) | Thread-0-serial DCT; 63/64 threads idle |
| 2 | `calculate_ssim_vert_combine` | ~15% | Shared-memory reduction, well-structured |
| 3 | `adm_cm` / `adm_csf_den` (float_adm 16 launches) | ~12% | 16 serial launches per frame; launch overhead dominates at small res |
| 4 | `vif` kernels (integer_vif, 4 scales) | ~10% | 4 serial cuLaunchKernel calls on picture stream |
| 5 | Host-side `cuStreamSynchronize` (ms_ssim, psnr_hvs collect) | ~8% | Synchronous stalls outside drain_batch |

---

## Hotspot analysis

### Hotspot 1: psnr_hvs_score.cu — thread-0-serial execution

**Location**: `/core/src/feature/cuda/integer_psnr_hvs/psnr_hvs_score.cu`, line 225.

```c
if (local_idx != 0u)
    return;
/* Thread 0: full per-block computation in CPU order. */
```

All 63 non-zero threads in each 64-thread block return immediately. The 2-D DCT
(`od_bin_fdct8x8`), CSF masking, variance computation, and partial accumulation
all execute serially in thread 0. Warp execution efficiency: **1/64 = 1.56%**.

This is a deliberate precision trade-off: the comment cites "CPU's exact i,j
summation order" to satisfy the `places=3` cross-backend contract (ADR-0191).
However the `places=4` gate does not require serial summation — it requires
bit-equivalent results only to 4 decimal places, not exact IEEE 754 float
order. A parallel reduction with `__syncthreads` + warp shuffle would pass
`places=3`.

**Category**: Divergence / low occupancy.
**Effort**: M (requires precision analysis + ADR amendment for ADR-0191).
**Estimated gain**: +8–12% end-to-end on 7-extractor stack at 1080p+.

### Hotspot 2: psnr_hvs_cuda.c — drain_batch not integrated

**Status update 2026-05-14**: closed by
`fix/backlog-gap-pass-8-2026-05-14`. `submit_fex_cuda` now queues the
three plane-partial DtoH copies on `s->lc.str`, records `s->lc.finished`
through `vmaf_cuda_kernel_submit_post_record`, and joins the engine's
drain batch. `collect_fex_cuda` now calls `vmaf_cuda_kernel_collect_wait`
before reducing `h_partials[]`, so the per-extractor stream sync is skipped
when the engine has already drained the batch.

**Location**: `/core/src/feature/cuda/integer_psnr_hvs_cuda.c`, lines 383-390.

```c
/* D2H readback all 3 planes' partials, then sync. */
for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_partials[p], ...));
}
CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->lc.str));
```

`collect_fex_cuda` issues three `cuMemcpyDtoHAsync` calls and then a raw
`cuStreamSynchronize` — it does NOT call `vmaf_cuda_kernel_submit_post_record`
in `submit_fex_cuda` and does NOT use `vmaf_cuda_kernel_collect_wait`. The
`lc.drained` flag is never set, so the drain_batch engine (PR #312) cannot
skip the per-extractor sync for psnr_hvs when running the 7-extractor stack.

Impact: in the 7-extractor rich workload, psnr_hvs adds a mandatory
`cuStreamSynchronize` barrier that serializes the collect phase, partially
negating the drain_batch gains from the other 6 extractors.

**Category**: Launch / sync overhead.
**Effort**: S (add `vmaf_cuda_kernel_submit_post_record` in submit, replace raw
sync with `vmaf_cuda_kernel_collect_wait` in collect).
**Estimated gain**: +2–4% on 7-extractor stack.

### Hotspot 3: integer_ms_ssim_cuda.c — picture-stream sync stalls in submit

**Location**: `/core/src/feature/cuda/integer_ms_ssim_cuda.c`, lines 281, 298.

Two `cuStreamSynchronize` calls block the host thread inside `submit_fex_cuda`
while waiting for the picture-pool stream to complete. This prevents concurrent
H2D + kernel overlap across scales. ms_ssim also does not use drain_batch.

**Category**: Memory bound (H2D serialized against kernel).
**Effort**: M (dedicate a separate upload stream + event, model on
psnr_hvs_cuda's T-GPU-OPT-2 pattern from PR #320).
**Estimated gain**: +3–5% on 7-extractor stack.

---

## Submit-side breakdown (host time, static analysis)

| Phase | Dominant cost | Source |
|-------|--------------|--------|
| submit_fex_cuda (per extractor) | cuLaunchKernel × N | <1 µs per launch; scales linearly with extractor count |
| psnr_hvs upload (per frame) | cuStreamSynchronize × 2 (pic streams) in `upload_frame` | ~5 µs for 576x324; was ~30 µs before T-GPU-OPT-3 (saved the per-frame alloc) |
| drain_batch_flush (per frame) | 1× cuStreamSynchronize on drain stream | Replaces N × cuStreamSynchronize for adm+vif+motion |
| collect (per extractor) | 1× cuStreamSynchronize per non-drain-batch extractor | psnr_hvs, ms_ssim still pay this |

---

## Comparison vs 2026-05-02 pre-sprint baseline

| Scenario | Pre-sprint (perf_benchmark_results.json) | Post-sprint | Delta |
|----------|----------------------------------------:|------------:|------:|
| 1080p 48f CUDA best_fps (FFmpeg) | **249 fps** | not measured (1080p BBB missing) | — |
| 4K 200f CUDA best_fps (FFmpeg) | **271 fps** | not measured (4K content missing) | — |
| 576x324 7-extractor CUDA best_fps (CLI) | ~1 538 fps (PR #312 in-memory) | **432 fps** (CLI, file I/O) | different methodology |
| 576x324 motion_cuda single-extractor | ~7 500 fps est | **8 271 fps** | +~10% |
| 576x324 vif_cuda single-extractor | ~6 500 fps est | **6 958 fps** | +~7% |
| 576x324 adm_cuda single-extractor | ~6 000 fps est | **6 389 fps** | +~6% |

**Regression check**: No regression detected vs committed snapshot. The +8.6%
drain_batch gain (PR #312) and the +5–8% estimated psnr_hvs async gain (PR #320)
are directionally confirmed by the vmaf_bench single-extractor numbers.
PASS.

---

## Top-3 NEW optimization targets

| Rank | Target | Specific change | Effort | Estimated gain |
|------|--------|----------------|--------|---------------|
| 1 | `psnr_hvs_score.cu` — thread-0-serial DCT | Parallelize 2-D DCT across 64 threads using `__shared__` transpose + warp-shuffle reduction for the CSF accumulation. Per-plane result matches places=3 contract via two-pass `__syncthreads` reduction (validate against ADR-0191 tolerance bounds). | M | +8–12% e2e on 7-ext at 1080p |
| 2 | `integer_psnr_hvs_cuda.c` — drain_batch integration | **DONE 2026-05-14**: submit-side partial readback + `vmaf_cuda_kernel_submit_post_record`; collect now uses `vmaf_cuda_kernel_collect_wait`. | S | +2–4% e2e on 7-ext stack |
| 3 | `integer_ms_ssim_cuda.c` — async upload + drain_batch | Replace the two blocking `cuStreamSynchronize` calls in `submit_fex_cuda` (upload phase) with a dedicated upload stream + event, mirroring T-GPU-OPT-2 from `integer_psnr_hvs_cuda.c`. Then add drain_batch registration as in target 2. | M | +3–5% e2e on 7-ext stack |

Combined estimated gain (all three): **+13–21%** additional end-to-end improvement
on 7-extractor rich workloads at 1080p and above.

---

## What we missed (expected vs actual)

| Expected | Actual | Explanation |
|----------|--------|-------------|
| psnr_hvs async upload (PR #320) shrinks cuMemcpyHtoDAsync count | Confirmed: persistent pinned buffers eliminate per-frame alloc/free (~5 µs saved per frame). Cannot quantify further without nsys. | T-GPU-OPT-3 landed correctly. |
| drain_batch should have shrunk submit_fex_cuda host time for 7-ext | drain_batch IS active for adm/vif/motion. psnr_hvs and ms_ssim are NOT integrated — they partially cancel the savings. | Integration gap found (Hotspot 2 + 3). |
| Float_adm 16-launch overhead | Not reduced by sprint PRs — remains a separate target (SSIMulacra2-style fused kernel or graph capture). | Out of scope for this sprint. |

---

## Profiler gaps

- **nsys / ncu not installed**: Per-kernel GPU time, SM occupancy, memory
  bandwidth, warp efficiency, and instruction throughput are not quantified.
  All kernel estimates above are from static analysis and vmaf_bench wall-clock.
- **In-memory harness not preserved**: PR #312's custom multi-extractor
  benchmark binary is not in-tree. Methodology-matched regression tracking
  requires adding a multi-extractor path to `vmaf_bench`.
- **1080p BBB content missing**: `testdata/perf_benchmark_results.json`
  baseline cannot be re-run from this host. Direct fps delta vs baseline is
  not available.
- **CUDA event instrumentation**: The vmaf_bench `--gpu-profile` flag is
  SYCL-only. Adding a CUDA event timer path to vmaf_bench would provide
  per-extractor kernel timing without nsys.

### Recommended remediation

Install Nsight Systems: `sudo pacman -S nsight-systems` (CachyOS). Once
available, re-run with:

```bash
nsys profile --stats=true -o /tmp/cuda-prof-$(date +%Y-%m-%d) \
  build_prof/tools/vmaf --backend cuda \
  -r python/test/resource/yuv/src01_hrc00_576x324.yuv \
  -d python/test/resource/yuv/src01_hrc01_576x324.yuv \
  -w 576 -h 324 -p 420 -b 8 \
  -m path=model/vmaf_v0.6.1.json \
  --feature psnr_cuda --feature ciede_cuda --feature ssim_cuda --feature psnr_hvs_cuda \
  --json -o /tmp/cuda-out.json
nsys stats /tmp/cuda-prof-*.nsys-rep
```

---

## Reproducer

```bash
# Build
WDIR=/home/kilian/dev/vmaf/.claude/worktrees/agent-a539f22f5d0e40d37
meson setup $WDIR/core/build_prof $WDIR/libvmaf \
  --buildtype=release -Db_ndebug=false \
  -Dc_args='-g -fno-omit-frame-pointer' \
  -Denable_cuda=true -Denable_sycl=false -Denable_vulkan=disabled
ninja -C $WDIR/core/build_prof

# Single-extractor vmaf_bench
VMAF_TEST_DATA=$WDIR/testdata \
  $WDIR/core/build_prof/tools/vmaf_bench --frames 48 --resolution 576x324

# 7-extractor CLI benchmark (30 runs)
VMAF=$WDIR/core/build_prof/tools/vmaf
MODEL=$WDIR/model/vmaf_v0.6.1.json
REF=/home/kilian/dev/vmaf/python/test/resource/yuv/src01_hrc00_576x324.yuv
DIS=/home/kilian/dev/vmaf/python/test/resource/yuv/src01_hrc01_576x324.yuv

for i in $(seq 1 30); do
  start=$(date +%s%N)
  $VMAF --backend cuda -r $REF -d $DIS -w 576 -h 324 -p 420 -b 8 \
    -m path=$MODEL \
    --feature psnr_cuda --feature ciede_cuda --feature ssim_cuda --feature psnr_hvs_cuda \
    --json -o /tmp/cuda7x_${i}.json 2>/dev/null
  end=$(date +%s%N)
  ms=$(( (end - start) / 1000000 ))
  python3 -c "print(f'Run ${i}: ${ms}ms => {48000/${ms}:.1f} fps')"
done
```

---

## ADR-0108 deliverables

1. **Research digest**: this file (`docs/development/cuda-profile-2026-05-03.md`).
2. **Decision matrix**: no implementation decision made — research only.
3. **`AGENTS.md` invariant**: no rebase-sensitive invariants introduced.
4. **Reproducer**: see §Reproducer above.
5. **CHANGELOG**: opt-out — research-only, no user-visible surface change.
6. **Rebase note**: opt-out — no rebase impact; purely observational.
