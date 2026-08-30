<!-- markdownlint-disable MD060 -->
# Research-0752 — Multi-Resolution Performance Benchmark Baseline

**Date:** 2026-05-29
**Author:** lusoris / Claude (Anthropic)
**ADR:** [ADR-0752](../adr/0752-perf-bench-multi-resolution.md)
**PR:** feat/perf-bench-multi-resolution-20260529

---

## 1. Motivation

Performance work across the fork (CUDA kernel tuning, AVX-512 SIMD, 1080p
re-measurement in research-0748) has been benchmarked ad-hoc.  Numbers are
scattered across PR descriptions, `testdata/bench_perf.py` output, and
`testdata/bench_all.sh` terminal logs.  No single authoritative JSON exists that
a future PR can diff against to detect regressions.

## 2. Methodology

### 2.1 Script design

`scripts/perf/bench-multi-resolution.sh` drives the `vmaf` binary directly
(not via FFmpeg) across the Cartesian product:

```text
resolutions × backends × metrics
```

For each cell: 3 timing runs, median wall time reported.  VMAF composite
score and the requested feature score are both captured from the output JSON.

### 2.2 Fixture strategy

| Key | Width | Height | Frames | Source |
|-----|-------|--------|--------|--------|
| 576 | 576 | 324 | 48 | Native — Netflix golden `testdata/ref_576x324_48f.yuv` |
| 720 | 640 | 480 | 48 | Native — `testdata/ref_640x480_48f.yuv` |
| 1080 | 1920 | 1080 | 48 | ffmpeg bilinear upscale from 576×324 |
| 1440 | 2560 | 1440 | 48 | ffmpeg bilinear upscale from 576×324 |
| 2160 | 3840 | 2160 | 30 | First 30 frames trimmed from `testdata/bbb/ref_3840x2160_200f.yuv` |

Upscaled fixtures are written once to `testdata/` and reused.  The
upscale method (bilinear, not reference-quality) means scores from 1080p/1440p
fixtures are not comparable to scores from natively-produced content; they
exist only as throughput vehicles.

### 2.3 Metric-to-feature mapping

| Metric arg | vmaf `--feature` flag | Pooled JSON key |
|------------|----------------------|-----------------|
| `vif`      | `vif`                | `vif_scale0`    |
| `adm`      | `adm`                | `adm2`          |
| `motion`   | `motion`             | `motion`        |
| `ssim`     | `float_ssim`         | `float_ssim`    |
| `ms_ssim`  | `float_ms_ssim`      | `float_ms_ssim` |

### 2.4 Backend engagement

Uses the same flag semantics as `testdata/bench_all.sh` (ADR-0513):

| Backend | Flags |
|---------|-------|
| cpu     | `--no_cuda --no_sycl --no_vulkan` |
| cuda    | `--gpumask=0 --no_sycl --no_vulkan` |
| sycl    | `--sycl_device=0 --no_cuda --no_vulkan` |

### 2.5 Container

One-off `docker run` against `vmaf-dev-mcp:cuda13.3` with:

```text
--gpus all --device /dev/dri --group-add 988 -v /dev/dri/by-path:/dev/dri/by-path:ro
```

Build: `meson setup build-bench core -Denable_cuda=true --buildtype=release`.

## 3. Baseline results summary

The initial baseline (`testdata/perf_multi_resolution.json`) was generated at
commit `master@8930853864` (as referenced by the task).  SYCL was not
available in this container session (Intel NEO ABI mismatch; see
`docs/rebase-notes.md §One-off container SYCL device-access pattern`).
4K (2160p) cells depend on `testdata/bbb/ref_3840x2160_200f.yuv` being
present (gitignored, large asset).

Refer to `testdata/perf_multi_resolution.json` for machine-readable numbers.
The JSON carries `hardware.git_hash` and `timestamp` for traceability.

## 4. Gaps

| Gap | Cause | Resolution |
|-----|-------|-----------|
| SYCL cells skip | Intel NEO ABI mismatch in this container session | Re-run with SYCL-capable container after NEO update |
| HIP cells not yet wired | No HIP resolutions in the script | Extend `BACKEND_FLAGS` when HIP backend lands |
| ncu metrics not collected | `--ncu` not passed in baseline run | Run with `--ncu` on a machine where Nsight Compute is installed |
| 2160p cells require large BBB asset | `testdata/bbb/` not committed | Cells emit `status=skip` with `skip_reason=fixture_unavailable` when absent |

## 5. Future workflow

For any PR that claims a performance improvement:

1. Re-run the script with the same `--backends` and `--resolutions` flags.
2. Diff the two JSONs:

   ```bash
   python3 - old.json new.json <<'PYEOF'
   import json, sys
   old = {(r["resolution"],r["backend"],r["metric"]): r for r in json.load(open(sys.argv[1]))["runs"]}
   new = {(r["resolution"],r["backend"],r["metric"]): r for r in json.load(open(sys.argv[2]))["runs"]}
   for key in sorted(new):
       o = old.get(key, {}); n = new[key]
       if n.get("fps") and o.get("fps"):
           delta = (n["fps"] - o["fps"]) / o["fps"] * 100
           print(f"{key[0]:>5}p/{key[1]:6}/{key[2]:8} {o['fps']:7.1f} -> {n['fps']:7.1f} fps  {delta:+.1f}%")
   PYEOF
   ```

3. Include the diff table in the PR description under "Performance delta".
4. Update `testdata/perf_multi_resolution.json` in the PR if the improvement is
   intentional (or document regression if it is a trade-off).
