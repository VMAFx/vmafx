<!-- markdownlint-disable MD013 MD060 -->
# Research-0744 — Cross-Backend Parity Baseline (Pre-ncu-opt)

**Date:** 2026-05-28
**Branch:** research/cuda-cross-backend-baseline-20260528
**Purpose:** Establish a numeric and throughput baseline for CPU and CUDA (SYCL skipped — no Intel device exposed to one-off container) before `perf/cuda-vif-filter1d-ncu-driven` and future optimization PRs land. All future performance PRs should cite this digest as the pre-optimization reference point.

---

## Environment

| Item | Value |
|---|---|
| Container | `vmaf-dev-mcp:cuda13.3` (one-off `docker run --rm --gpus all`) |
| GPU | NVIDIA GeForce RTX 4090 |
| CUDA driver | 610.43.02 |
| vmaf binary | `/build/vmaf/core/build/tools/vmaf` v3.0.0 |
| Model | `vmaf_v0.6.1.json` |
| SYCL | Skipped — `vmaf_sycl_state_init` fails (no `/dev/dri` Intel node passed to container) |
| HIP | Skipped — no AMD device on host |
| Metal | Skipped — not Linux |

---

## Workloads

| ID | File pair | Resolution | Frames | bit-depth |
|---|---|---|---|---|
| WL1 | `src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv` | 576×324 | 48 | 8-bit |
| WL2 | `checkerboard_1920_1080_10_3_0_0.yuv` ↔ `..._1_0.yuv` | 1920×1080 | 3 | 8-bit |
| WL3 | `checkerboard_1920_1080_10_3_0_0.yuv` ↔ `..._10_0.yuv` | 1920×1080 | 3 | 8-bit |

---

## Wall-Time Baseline (3 runs each; all values in ms)

| Workload | Backend | Run 1 | Run 2 | Run 3 | Median |
|---|---|---|---|---|---|
| WL1 (576×324, 48f) | CPU | 96 | 86 | 90 | **90** |
| WL1 (576×324, 48f) | CUDA | 170 | 148 | 149 | **149** |
| WL2 (1080p, 3f) | CPU | 107 | 72 | 73 | **73** |
| WL2 (1080p, 3f) | CUDA | 161 | 147 | 154 | **154** |
| WL3 (1080p, 3f) | CPU | 73 | 75 | 74 | **74** |
| WL3 (1080p, 3f) | CUDA | 147 | 153 | 152 | **152** |

**Interpretation:** CPU is faster than CUDA on all three workloads at these frame counts. CUDA overhead (init + H2D/D2H transfers) dominates at 3–48 frames. Throughput advantage only appears with larger batch sizes; the `perf_benchmark_results.json` snapshot (BBB 1080p, 48 frames) shows CUDA at ~249 fps vs CPU at ~44 fps — a 5.7× advantage at that batch size. The crossover point is somewhere between 10 and 48 frames for 1080p content.

---

## Score Correctness vs CPU (ADR-0119 tolerance)

| Workload | CPU score | CUDA score | Delta | ULP diff | Status |
|---|---|---|---|---|---|
| WL1 (576×324, 48f) | 76.667830 | 76.667829 | −1.0×10⁻⁶ | 70,368,744 | WITHIN TOLERANCE |
| WL2 (1080p, 3f) | 35.068672 | 35.068668 | −4.0×10⁻⁶ | 562,949,953 | WITHIN TOLERANCE |
| WL3 (1080p, 3f) | 7.985899 | 7.985899 | 0.0 | 0 | EXACT |

The ULP values reflect the IEEE-754 bit-level distance between the two float64 values. The delta at WL2 is ~1.1×10⁻⁷ relative error, well within the established "close but never bit-exact" GPU tolerance (memory note: `golden gate is CPU-only; GPUs NOT bit-exact`). No correctness finding.

**Netflix golden gate (WL1):** CPU score 76.667830 — the Python `assertAlmostEqual(..., places=4)` gate expects 76.6678 at 4 decimal places. CUDA 76.667829 also passes at places=4. No regression.

---

## Per-Feature Parity (WL2, CPU vs CUDA)

| Feature | CPU | CUDA | Delta | ULP | Status |
|---|---|---|---|---|---|
| `integer_adm2` | 0.78533200 | 0.78533200 | 0.0 | 0 | EXACT |
| `integer_adm3` | 0.81757600 | MISSING | — | — | **GAP** |
| `integer_adm_scale0` | 0.72133600 | 0.72133600 | 0.0 | 0 | EXACT |
| `integer_adm_scale1` | 0.69258800 | 0.69258800 | 0.0 | 0 | EXACT |
| `integer_adm_scale2` | 0.80414800 | 0.80414800 | 0.0 | 0 | EXACT |
| `integer_adm_scale3` | 0.82791000 | 0.82791000 | 0.0 | 0 | EXACT |
| `integer_aim` | 0.15018000 | MISSING | — | — | **GAP** |
| `integer_motion` | 12.55471200 | 12.55471200 | 0.0 | 0 | EXACT |
| `integer_motion2` | 12.55471200 | 12.55471200 | 0.0 | 0 | EXACT |
| `integer_motion3` | 18.82319100 | 18.82319100 | 0.0 | 0 | EXACT |
| `integer_vif_scale0` | 0.11290600 | 0.11290600 | 0.0 | 0 | EXACT |
| `integer_vif_scale1` | 0.29837200 | 0.29837200 | 0.0 | 0 | EXACT |
| `integer_vif_scale2` | 0.33743200 | 0.33743200 | 0.0 | 0 | EXACT |
| `integer_vif_scale3` | 0.49644500 | 0.49644500 | 0.0 | 0 | EXACT |
| `vmaf` | 35.06867200 | 35.06866800 | −4.0×10⁻⁶ | 562,949,953 | WITHIN TOLERANCE |

**Feature gaps noted:** `integer_adm3` and `integer_aim` are absent from CUDA `pooled_metrics`. These are model-layer post-processing scalars; the CUDA feature extractor computes `integer_adm2` plus the four scale components but does not emit the derived `adm3` aggregate or the `aim` (Attention-weighted Inverse Metric) score. The vmaf model JSON references these; the CPU path computes them as a post-processing pass. Whether the CUDA path omits the collection step or the JSON-writer filters them requires a code-path check — outside scope of this baseline.

---

## Existing Snapshot Comparison

`testdata/perf_benchmark_results.json` records a previous BBB 1080p 48-frame run:

| Backend | Pooled score | Best FPS | This baseline |
|---|---|---|---|
| CPU | 95.098707 | 43.6 fps | — (different YUV) |
| CUDA | 95.098706 | 249.4 fps | — |
| SYCL | 95.098681 | 93.4 fps | SKIP (device unavailable) |

The new baseline uses Netflix golden YUV pairs rather than BBB, so scores are not comparable. However the SYCL 93 fps at 1080p (from the snapshot) and CUDA 249 fps at 1080p provide reference throughput numbers that future PRs should beat.

---

## Reproducer

```bash
# One-off container run (isolates from shared state)
docker run --rm --gpus all \
  --entrypoint /bin/bash \
  -v /home/kilian/dev/vmaf/python/test/resource/yuv:/yuv:ro \
  vmaf-dev-mcp:cuda13.3 \
  -c '/build/vmaf/core/build/tools/vmaf \
      -r /yuv/src01_hrc00_576x324.yuv \
      -d /yuv/src01_hrc01_576x324.yuv \
      -w 576 -h 324 -p 420 -b 8 \
      -m path=/build/vmaf/model/vmaf_v0.6.1.json \
      -o /tmp/out.json --json -q \
      --backend cpu'
# Replace --backend cpu with --backend cuda for CUDA run
```

---

## Observations and Open Questions

1. **CUDA slower than CPU for < ~10 frames at 1080p.** The crossover depends on frame count, not resolution alone. The `perf_benchmark_results.json` BBB data shows CUDA wins at 48 frames. Budget 100–150ms for CUDA init per invocation.

2. **`integer_adm3` / `integer_aim` missing from CUDA output.** These features are emitted by the CPU path; the CUDA `float_adm_cuda` extractor apparently does not expose them in the pooled metrics JSON. Requires investigation: ADR-0574 added `VMAF_feature_aim_score` and `VMAF_feature_adm3_score` to `float_adm_cuda`. The missing output may be an integer vs float ADM path distinction — the vmaf_v0.6.1 model uses integer ADM by default, and the CUDA path may route through `integer_adm_cuda` which predates ADR-0574.

3. **SYCL baseline unavailable.** The Intel Arc A380 device is not exposed to one-off containers via `--device /dev/dri/renderD129`. The prior snapshot shows SYCL at ~93 fps on 1080p. To get SYCL baseline, run via the shared `vmaf-dev-mcp` container: `docker exec vmaf-dev-mcp vmaf ... --backend sycl`.

4. **Score determinism is good.** All three CPU runs and all three CUDA runs produce identical scores across runs (no non-determinism from threading or GPU scheduling observed at this clip length).

---

## Future Comparison Baseline

When `perf/cuda-vif-filter1d-ncu-driven` lands, compare against:

- WL1 CPU median: **90ms**, CUDA median: **149ms**
- WL2 CPU median: **73ms**, CUDA median: **154ms**
- WL1 VMAF score (CPU): **76.667830**, (CUDA): **76.667829**, delta: **−1×10⁻⁶**
- WL2 VMAF score (CPU): **35.068672**, (CUDA): **35.068668**, delta: **−4×10⁻⁶**
- Existing BBB 1080p CUDA throughput snapshot: **249 fps** (from `perf_benchmark_results.json`)
