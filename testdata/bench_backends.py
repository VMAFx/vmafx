#!/usr/bin/env python3
"""Per-backend performance baseline harness (repetition + median).

Companion to ``testdata/bench_all.sh``. ``bench_all.sh`` answers "do the
backends still agree numerically?" with one timed run per cell; this script
answers "how fast is each backend *right now*?" and therefore takes the
statistics seriously:

* every cell is run ``--runs`` times after one discarded warmup;
* the reported figure is the **median**, with min/max spread alongside;
* the 1-minute load average is sampled before and after every cell, because
  a benchmark on a shared workstation is only interpretable next to the load
  it ran under;
* exactly one backend is engaged per run via the exclusive ``--backend``
  selector, so two GPU jobs never overlap.

The emitted JSON reuses the key shape of ``testdata/perf_benchmark_results.json``
(``pooled`` / ``nframes`` / ``times`` / ``best_fps`` / ``avg_fps`` /
``best_time``) and adds ``median_fps`` / ``median_time`` / ``metrics_keys`` /
``load_avg_1min``.

Backend engagement is verified per cell by the ``frames[0].metrics`` key count
(see ``core/AGENTS.md`` §"Backend-engagement foot-guns"): a GPU row whose key
count equals the CPU row's is a silent CPU fallback, not a GPU result.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time

# --- Fixture table -------------------------------------------------------
# (tag, ref, dis, width, height, bitdepth, human label)
# Paths are relative to the repo root; the 4K pair is gitignored and must be
# generated first (see docs/development/backend-perf-baselines.md).
FIXTURES = [
    (
        "src01_576x324",
        "python/test/resource/yuv/src01_hrc00_576x324.yuv",
        "python/test/resource/yuv/src01_hrc01_576x324.yuv",
        576,
        324,
        8,
        "Netflix src01 pair, 576x324, 48f",
    ),
    (
        "checkerboard_1px",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv",
        1920,
        1080,
        8,
        "Checkerboard 1-px shift, 1920x1080, 3f",
    ),
    (
        "checkerboard_10px",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_10_0.yuv",
        1920,
        1080,
        8,
        "Checkerboard 10-px shift, 1920x1080, 3f",
    ),
    (
        "bbb_4k_200f",
        "testdata/bbb/ref_3840x2160_200f.yuv",
        "testdata/bbb/dis_3840x2160_200f.yuv",
        3840,
        2160,
        8,
        "BBB 4K, 3840x2160, 200f",
    ),
]

# "default" means: pass no --model at all, so libvmaf resolves
# VMAF_DEFAULT_MODEL_VERSION (vmaf_v1.0.16_3d0h). See
# docs/development/default-model.md.
MODELS = {
    "v0.6.1": "model/vmaf_v0.6.1.json",
    "default": None,
}


def load1() -> float:
    with open("/proc/loadavg", encoding="ascii") as fh:
        return float(fh.read().split()[0])


def build_cmd(vmaf_bin, fixture, backend, model_path, out_path, threads):
    _tag, ref, dis, w, h, bd, _label = fixture
    cmd = [
        vmaf_bin,
        "--reference",
        ref,
        "--distorted",
        dis,
        "--width",
        str(w),
        "--height",
        str(h),
        "--pixel_format",
        "420",
        "--bitdepth",
        str(bd),
        "--threads",
        str(threads),
        "--backend",
        backend,
        "--output",
        out_path,
        "--json",
        "-q",
    ]
    if model_path is not None:
        cmd += ["--model", f"path={model_path}"]
    return cmd


def run_cell(vmaf_bin, fixture, backend, model_name, runs, threads, verbose):
    """Time one (fixture, backend, model) cell. Returns a result dict or None."""
    model_path = MODELS[model_name]
    times = []
    pooled = None
    nframes = None
    metrics_keys = None
    load_before = load1()

    with tempfile.TemporaryDirectory(prefix="vmaf-bench-") as td:
        out_path = os.path.join(td, "out.json")
        cmd = build_cmd(vmaf_bin, fixture, backend, model_path, out_path, threads)
        # runs + 1: the first iteration is a discarded warmup (page cache,
        # GPU context creation, JIT of SYCL kernels).
        for i in range(runs + 1):
            start = time.monotonic()
            proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
            elapsed = time.monotonic() - start
            if proc.returncode != 0 or not os.path.exists(out_path):
                msg = (proc.stderr or proc.stdout or "").strip().splitlines()
                return {
                    "status": "unavailable",
                    "returncode": proc.returncode,
                    "error": msg[-1] if msg else "no output file produced",
                }
            if i == 0:
                continue  # warmup
            times.append(elapsed)
            if pooled is None:
                with open(out_path, encoding="utf-8") as fh:
                    doc = json.load(fh)
                pooled = doc["pooled_metrics"]["vmaf"]["mean"]
                nframes = len(doc["frames"])
                metrics_keys = len(doc["frames"][0]["metrics"])
            if verbose:
                print(f"      run {i}: {elapsed:.3f}s", file=sys.stderr)

    load_after = load1()
    med = statistics.median(times)
    return {
        "status": "ok",
        "pooled": pooled,
        "nframes": nframes,
        "metrics_keys": metrics_keys,
        "times": times,
        "median_time": med,
        "best_time": min(times),
        "median_fps": nframes / med,
        "best_fps": nframes / min(times),
        "avg_fps": nframes / statistics.fmean(times),
        "spread_pct": (max(times) - min(times)) / med * 100.0,
        "load_avg_1min": [load_before, load_after],
    }


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--vmaf-bin", default=os.environ.get("VMAF_BIN", "core/build/tools/vmaf"))
    ap.add_argument(
        "--runs",
        type=int,
        default=int(os.environ.get("VMAF_BENCH_RUNS", "3")),
        help="timed repetitions per cell, after one discarded warmup (default 3)",
    )
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument(
        "--backend", action="append", dest="backends", help="repeatable; default cpu,cuda,sycl,hip"
    )
    ap.add_argument(
        "--model",
        action="append",
        dest="models",
        choices=sorted(MODELS),
        help="repeatable; default both",
    )
    ap.add_argument(
        "--fixture",
        action="append",
        dest="fixtures",
        help="repeatable fixture tag; default all present ones",
    )
    ap.add_argument("--json-out", default=None)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    backends = args.backends or ["cpu", "cuda", "sycl", "hip"]
    models = args.models or ["v0.6.1", "default"]

    root = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=False
    ).stdout.strip()
    if root:
        os.chdir(root)

    fixtures = [f for f in FIXTURES if args.fixtures is None or f[0] in args.fixtures]
    missing = [f[0] for f in fixtures if not (os.path.exists(f[1]) and os.path.exists(f[2]))]
    if missing:
        print(f"note: skipping fixtures with absent files: {', '.join(missing)}", file=sys.stderr)
    fixtures = [f for f in fixtures if os.path.exists(f[1]) and os.path.exists(f[2])]
    if not fixtures:
        print("error: no fixture files present", file=sys.stderr)
        return 2

    if args.dry_run:
        for fx in fixtures:
            for mn in models:
                for be in backends:
                    with tempfile.TemporaryDirectory() as td:
                        cmd = build_cmd(
                            args.vmaf_bin,
                            fx,
                            be,
                            MODELS[mn],
                            os.path.join(td, "out.json"),
                            args.threads,
                        )
                    print(" ".join(cmd))
        return 0

    results: dict = {}
    print(
        f"# vmaf backend baselines — {args.runs} timed runs/cell + 1 warmup, "
        f"threads={args.threads}",
        flush=True,
    )
    print(f"# load average at start: {load1():.2f}", flush=True)
    for fx in fixtures:
        tag, _ref, _dis, _w, _h, _bd, label = fx
        results[tag] = {"label": label}
        print(f"\n## {label}", flush=True)
        for mn in models:
            results[tag][mn] = {}
            print(f"  model={mn}", flush=True)
            for be in backends:
                print(f"    {be:6s} ... ", end="", flush=True)
                res = run_cell(args.vmaf_bin, fx, be, mn, args.runs, args.threads, args.verbose)
                results[tag][mn][be] = res
                if res["status"] != "ok":
                    print(f"SKIP ({res['error']})", flush=True)
                    continue
                print(
                    f"{res['median_fps']:8.2f} fps  median {res['median_time']:.3f}s  "
                    f"spread {res['spread_pct']:.1f}%  pool {res['pooled']:.6f}  "
                    f"keys={res['metrics_keys']}  load={res['load_avg_1min'][1]:.1f}",
                    flush=True,
                )

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump(results, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print(f"\nwrote {args.json_out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
