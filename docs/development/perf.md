# Performance Benchmarking

This page describes how to benchmark VMAF throughput across resolutions,
backends, and metrics, and how to use the versioned JSON baseline for
regression detection.

## Quick start

```bash
# CPU-only (no GPU required)
bash scripts/perf/bench-multi-resolution.sh \
  --backends cpu \
  --resolutions 576,720,1080 \
  --metrics vif,adm,motion,ssim,ms_ssim \
  --output /tmp/my_perf.json

# CPU + CUDA inside the dev container
docker run --rm --gpus all \
  -v $(git rev-parse --show-toplevel):/workspace \
  -w /workspace \
  vmaf-dev-mcp:cuda13.3 bash -c '
    export VMAF_BIN=/workspace/core/build/tools/vmaf
    bash scripts/perf/bench-multi-resolution.sh \
      --backends cpu,cuda \
      --output testdata/perf_multi_resolution.json
  '
```

## Script reference

```
scripts/perf/bench-multi-resolution.sh [OPTIONS]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--backends` | `cpu` | Comma-separated list: `cpu`, `cuda`, `sycl` |
| `--resolutions` | all | Comma-separated height keys: `576`, `720`, `1080`, `1440`, `2160` |
| `--metrics` | all | `vif`, `adm`, `motion`, `ssim`, `ms_ssim` |
| `--runs` | `3` | Timing runs per cell; median is kept |
| `--ncu` | off | Collect Nsight Compute metrics (CUDA cells only) |
| `--output` | `testdata/perf_multi_resolution.json` | Output JSON path |
| `--workspace` | git toplevel | Root of the VMAF tree |
| `--dry-run` | off | Print plan and exit without running |

### Environment variables

| Variable | Purpose |
|----------|---------|
| `VMAF_BIN` | Override vmaf binary path |
| `VMAF_CUDA_HOME` | Override CUDA install prefix (fallback: `$CUDA_HOME`, `/opt/cuda`, `/usr/local/cuda`) |
| `VMAF_ONEAPI_SETVARS` | Override oneAPI `setvars.sh` path |
| `VMAF_NCU` | Override `ncu` binary path |

## JSON schema

```jsonc
{
  "schema_version": "1",
  "timestamp": "2026-05-29T...",
  "hardware": {
    "cpu_model": "...",
    "gpu_model": "...",
    "nvidia_driver": "...",
    "cuda_version": "...",
    "git_hash": "..."
  },
  "toolkit": {
    "runs_per_cell": 3,
    "timing": "median wall time (ms)",
    "ncu_enabled": false
  },
  "fixture_notes": { ... },
  "total_cells": 50,
  "ok_cells": 40,
  "skipped_cells": 10,
  "runs": [
    {
      "resolution": "1080",
      "backend": "cuda",
      "metric": "adm",
      "width": 1920,
      "height": 1080,
      "bitdepth": 8,
      "frames": 48,
      "fixture_source": "upscaled",
      "median_ms": 142,
      "fps": 338.0,
      "vmaf_score": 74.123456,
      "feature_score": 10.987,
      "status": "ok",
      "skip_reason": null,
      "ncu": {}
    }
  ]
}
```

`status` is `"ok"` or `"skip"`.  `skip_reason` is non-null when `status=skip`.
`ncu` is populated only when `--ncu` is passed and the backend is `cuda`.

## Fixtures

| Key | Size | Source |
|-----|------|--------|
| `576` | 576×324, 48f | Native — Netflix golden `testdata/ref_576x324_48f.yuv` |
| `720` | 640×480, 48f | Native — `testdata/ref_640x480_48f.yuv` |
| `1080` | 1920×1080, 48f | Generated on first run — ffmpeg bilinear upscale from 576×324 |
| `1440` | 2560×1440, 48f | Generated on first run — ffmpeg bilinear upscale from 576×324 |
| `2160` | 3840×2160, 30f | Trimmed from `testdata/bbb/ref_3840x2160_200f.yuv` (gitignored) |

Upscaled fixtures are cached in `testdata/` after the first run.  The upscale
is bilinear (throughput vehicle only; scores from these fixtures are not
comparable to production content).

## Comparing a PR against the baseline

Run the script before and after your change, then diff:

```python
import json, sys
old = {(r["resolution"],r["backend"],r["metric"]): r
       for r in json.load(open(sys.argv[1]))["runs"]}
new = {(r["resolution"],r["backend"],r["metric"]): r
       for r in json.load(open(sys.argv[2]))["runs"]}
for key in sorted(new):
    o = old.get(key, {}); n = new[key]
    if n.get("fps") and o.get("fps"):
        delta = (n["fps"] - o["fps"]) / o["fps"] * 100
        print(f"{key[0]:>5}p/{key[1]:6}/{key[2]:8} "
              f"{o['fps']:7.1f} -> {n['fps']:7.1f} fps  {delta:+.1f}%")
```

Include the diff table in the PR description under "Performance delta".
If the PR intentionally improves performance, commit the updated
`testdata/perf_multi_resolution.json`.

## SYCL prerequisites

SYCL cells require oneAPI to be sourced inside the execution environment.
The script attempts to source `setvars.sh` from `$VMAF_ONEAPI_SETVARS`,
`/opt/intel/oneapi-2025.3/setvars.sh`, and `/opt/intel/oneapi/setvars.sh`
in that order.  If none are found, SYCL cells emit `status=skip`.

See also the one-off container SYCL device-access pattern in
[`docs/rebase-notes.md`](../rebase-notes.md).

## ncu integration

Pass `--ncu` to collect Nsight Compute metrics for CUDA cells.
The following counters are collected per kernel:

- `sm__throughput.avg.pct_of_peak_sustained_elapsed` — SM utilisation
- `l1tex__t_sector_hit_rate.pct` — L1 cache hit rate
- `lts__t_sector_hit_rate.pct` — L2 cache hit rate
- `gpu__time_duration.sum` — total GPU time

Requires `ncu` (Nsight Compute ≥ 2022) on `PATH` or set via `$VMAF_NCU`.

## Baseline location

The versioned baseline lives at `testdata/perf_multi_resolution.json`.
The `hardware.git_hash` and `timestamp` fields identify when and on what
machine it was generated.  Regenerate intentionally via the script after any
structural performance change; include the justification in the commit message.
