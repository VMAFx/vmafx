# testdata/

Fork-added fixtures, benchmark scripts, and numerical snapshot baselines.
Netflix golden data lives in `python/test/resource/yuv/` — do not mix.

## YUV fixtures (committed, 70 MB total)

| File | Resolution | Frames | Bit-depth | Used by |
|------|-----------|--------|-----------|---------|
| `ref_576x324_48f.yuv` | 576×324 | 48 | 8 | CI snapshot gate, `core/test/test_mcp_smoke.c`, `scripts/perf/bench-multi-resolution.sh` |
| `dis_576x324_48f.yuv` | 576×324 | 48 | 8 | same |
| `ref_640x480_48f.yuv` | 640×480 | 48 | 8 | `scripts/perf/bench-multi-resolution.sh` (720-tier) |
| `dis_640x480_48f.yuv` | 640×480 | 48 | 8 | same |

Higher-resolution fixtures (1080p, 1440p, 4K) are **gitignored**; generate
them with `./generate.sh` — see that script for source requirements.

## Numerical snapshot JSONs (committed)

Regenerate intentionally with `/regen-snapshots`; include a justification.

| File | Backend | Resolution | Notes |
|------|---------|-----------|-------|
| `scores_cpu_576.json` | CPU | 576×324 | |
| `scores_cpu_640.json` | CPU | 640×480 | |
| `scores_cpu_720.json` | CPU | 640×480 up-labeled | generated against 640×480 fixture |
| `scores_cpu_1080.json` | CPU | 1920×1080 | uses upscaled fixture |
| `scores_cpu_4k.json` | CPU | 3840×2160 | uses bbb fixture |
| `scores_sycl_a380_*.json` | SYCL / Intel Arc A380 | 576–4K | |
| `scores_sycl_b580_*.json` | SYCL / Intel Arc B580 | 576–4K | |
| `scores_sycl_uhd770_*.json` | SYCL / Intel UHD 770 | 576–4K | |
| `perf_multi_resolution.json` | CPU + CUDA | 576–1440 | multi-resolution throughput baseline (ADR-0752) |

## Scripts

| File | Purpose |
|------|---------|
| `generate.sh` | Generate gitignored large-resolution YUV pairs from Big Buck Bunny source |
| `gen_cpu_golden.py` | Regenerate `scores_cpu_*.json` snapshots |
| `run_sycl_scores.py` | Regenerate `scores_sycl_<gpu>_*.json` snapshots |
| `benchmark_netflix.py` | Run the Netflix benchmark suite; output goes to `netflix_benchmark_results.json` (gitignored) |
| `bench_perf.py` | Detailed per-backend per-resolution throughput benchmark; output goes to `perf_benchmark_results.json` (gitignored) |
| `bench_quick.py` | Quick 3-run FPS check across resolutions |
| `bench_all.sh` | Per-backend sweep across all canonical fixture sizes; also used by CI `--snapshot-only` gate |
| `test_all_backends.sh` | Interactive validation: score + FPS for CPU/CUDA/SYCL |
| `test_bench_perf.py` | Unit tests for `bench_perf.py` |
| `compare_a380.py` | CPU vs SYCL/A380 score diff report |
| `compare_combined.py` | CPU vs SYCL/A380 score diff report (all resolutions) |
| `check_borders.py` | Border-pixel sanity check on YUV fixtures |

## Ad-hoc benchmark outputs (gitignored)

`netflix_benchmark_results.json` and `perf_benchmark_results.json` are
outputs from manual benchmark runs. They are gitignored to avoid
accidental commits of stale machine-specific data (see CLAUDE.md §12 r5
and ADR-0813). Regenerate locally when needed; do not commit unless the
run is a formal baseline update.

## What does NOT belong here

- Netflix golden YUVs (`python/test/resource/yuv/`) — never move these.
- Large corpus data (37+ GB Netflix training set) — lives in `.corpus/` (gitignored).
- Model files — live in `model/`.
