#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

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

Backend engagement is verified per cell by inspecting ``backend_used`` and
``feature_backends``: a device row where ``backend_used`` differs from requested
or any extractor failed to run on device is flagged as ``fallback`` and publishes
no FPS.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import shutil
import sys
import tempfile
import time
from pathlib import Path
from typing import NamedTuple

_REPO_ROOT = Path(__file__).resolve().parents[1]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402


class Fixture(NamedTuple):
    tag: str
    ref: str
    dis: str
    width: int
    height: int
    pixel_format: str
    bitdepth: int
    color_args: list[str]
    label: str


COLOR_ARGS_PQ = [
    "--color_range_ref",
    "limited",
    "--color_primaries_ref",
    "bt2020",
    "--color_trc_ref",
    "smpte2084",
    "--color_matrix_ref",
    "bt2020nc",
    "--color_range_dist",
    "limited",
    "--color_primaries_dist",
    "bt2020",
    "--color_trc_dist",
    "smpte2084",
    "--color_matrix_dist",
    "bt2020nc",
]

# --- Fixture table -------------------------------------------------------
FIXTURES = [
    Fixture(
        "src01_576x324",
        "python/test/resource/yuv/src01_hrc00_576x324.yuv",
        "python/test/resource/yuv/src01_hrc01_576x324.yuv",
        576,
        324,
        "420",
        8,
        [],
        "Netflix src01 pair, 576x324, 48f",
    ),
    Fixture(
        "checkerboard_1px",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv",
        1920,
        1080,
        "420",
        8,
        [],
        "Checkerboard 1-px shift, 1920x1080, 3f",
    ),
    Fixture(
        "checkerboard_10px",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_10_0.yuv",
        1920,
        1080,
        "420",
        8,
        [],
        "Checkerboard 10-px shift, 1920x1080, 3f",
    ),
    Fixture(
        "bbb_4k_200f",
        "testdata/bbb/ref_3840x2160_200f.yuv",
        "testdata/bbb/dis_3840x2160_200f.yuv",
        3840,
        2160,
        "420",
        8,
        [],
        "BBB 4K, 3840x2160, 200f",
    ),
    Fixture(
        "sollevante_pq_1080p",
        "python/test/resource/yuv/pq-sollevante-v1/sollevante_ref_1920x1080_8f.yuv",
        "python/test/resource/yuv/pq-sollevante-v1/sollevante_dis_1920x1080_8f.yuv",
        1920,
        1080,
        "420",
        10,
        COLOR_ARGS_PQ,
        "Sol Levante PQ, 1920x1080, 8f",
    ),
]

NEWEST_SDR_MODELS = [
    "vmaf_v1.0.16_1d5h_2160",
    "vmaf_v1.0.16_3d0h",
    "vmaf_v1.0.16_3d0h_2160",
    "vmaf_v1.0.16_5d0h",
    "vmaf_v1.0.16_hfr_1d5h_2160",
    "vmaf_v1.0.16_hfr_3d0h",
    "vmaf_v1.0.16_hfr_3d0h_2160",
    "vmaf_v1.0.16_hfr_5d0h",
]

NEWEST_HDR_MODELS = [
    "hdr_vmaf_v1.1.8_1d5h_2160",
    "hdr_vmaf_v1.1.8_3d0h",
    "hdr_vmaf_v1.1.8_3d0h_2160",
    "hdr_vmaf_v1.1.8_5d0h",
    "hdr_vmaf_v1.1.8_hfr_1d5h_2160",
    "hdr_vmaf_v1.1.8_hfr_3d0h",
    "hdr_vmaf_v1.1.8_hfr_3d0h_2160",
    "hdr_vmaf_v1.1.8_hfr_5d0h",
]

NEWEST_MODELS = NEWEST_SDR_MODELS + NEWEST_HDR_MODELS

MODELS = {
    "v0.6.1": "path=model/vmaf_v0.6.1.json",
    "default": None,
    **{m: f"version={m}" for m in NEWEST_MODELS},
}


def load1() -> float:
    with open("/proc/loadavg", encoding="ascii") as fh:
        return float(fh.read().split()[0])


def cuda_free_mib() -> int:
    try:
        nvidia_smi = shutil.which("nvidia-smi")
        if nvidia_smi is None:
            return 0
        proc = run_command(
            [nvidia_smi, "--query-gpu=memory.free", "--format=csv,noheader,nounits"],
            allowed_executables=[nvidia_smi],
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=10,
        )
        if proc.returncode == 0 and proc.stdout.strip():
            return int(proc.stdout.strip().splitlines()[0])
    except Exception:
        pass
    return 0


def is_hdr_model(model_name: str) -> bool:
    return model_name.startswith("hdr_")


def is_pq_fixture(fixture: Fixture) -> bool:
    return bool(fixture.color_args) or "pq" in fixture.tag


def can_pair(fixture: Fixture, model_name: str) -> bool:
    # Rule: The libvmaf CLI strictly refuses running HDR models on SDR fixtures
    # without source colorimetry, returning -ERANGE / -EINVAL. The harness must
    # never pair them: HDR models run only on PQ fixtures, SDR models only on
    # SDR fixtures.
    if is_hdr_model(model_name):
        return is_pq_fixture(fixture)
    return not is_pq_fixture(fixture)


def build_cmd(
    vmaf_bin: str,
    fixture: Fixture,
    backend: str,
    model_name: str,
    out_path: str,
    threads: int,
) -> list[str]:
    if not can_pair(fixture, model_name):
        raise ValueError(
            f"Cannot pair HDR model '{model_name}' with SDR fixture '{fixture.tag}' "
            f"or SDR model with PQ fixture."
        )
    cmd = [
        vmaf_bin,
        "--reference",
        fixture.ref,
        "--distorted",
        fixture.dis,
        "--width",
        str(fixture.width),
        "--height",
        str(fixture.height),
        "--pixel_format",
        fixture.pixel_format,
        "--bitdepth",
        str(fixture.bitdepth),
        "--threads",
        str(threads),
        "--backend",
        backend,
        "--output",
        out_path,
        "--json",
        "-q",
    ]
    if fixture.color_args:
        cmd.extend(fixture.color_args)
    spec = MODELS.get(model_name)
    if spec is not None:
        cmd.extend(["--model", spec])
    return cmd


def check_engagement(doc: dict, requested_backend: str) -> tuple[bool, str | None]:
    if requested_backend == "cpu":
        return True, None
    backend_used = doc.get("backend_used")
    if backend_used != requested_backend:
        return False, f"backend_used ({backend_used}) != requested ({requested_backend})"
    feature_backends = doc.get("feature_backends") or []
    not_on_device = [
        f"{fb.get('extractor')}={fb.get('backend')}"
        for fb in feature_backends
        if fb.get("backend") != requested_backend
    ]
    if not_on_device:
        return False, f"extractors not on device: {', '.join(not_on_device)}"
    return True, None


def resolve_executable(name: str) -> str:
    """Return `name` as run_command accepts it: bare names stay, paths become absolute.

    The default `--vmaf-bin` is `core/build/tools/vmaf`, relative to the repository root;
    run_command refuses relative paths with a directory part.
    """
    if os.sep not in name and (os.altsep is None or os.altsep not in name):
        return name
    path = Path(name)
    return str(path if path.is_absolute() else (_REPO_ROOT / path).resolve())


def execute_run(cmd: list[str], out_path: str) -> tuple[int, float, str, dict | None]:
    cmd = [resolve_executable(cmd[0]), *cmd[1:]]
    start = time.monotonic()
    proc = run_command(
        cmd,
        capture_output=True,
        text=True,
        check=False,
        allowed_executables=[cmd[0]],
        timeout_seconds=3600,
        max_output_bytes=64 * 1_048_576,
    )
    elapsed = time.monotonic() - start
    if proc.returncode != 0 or not os.path.exists(out_path):
        msg = (proc.stderr or proc.stdout or "").strip().splitlines()
        err = msg[-1] if msg else "no output file produced"
        return proc.returncode, elapsed, err, None
    with open(out_path, encoding="utf-8") as fh:
        doc = json.load(fh)
    return 0, elapsed, "", doc


def run_cell_iterations(
    cmd: list[str],
    out_path: str,
    runs: int,
    verbose: bool,
) -> tuple[list[float], dict | None, dict | None]:
    times = []
    first_doc = None
    for i in range(runs + 1):
        rc, elapsed, err, doc = execute_run(cmd, out_path)
        if rc != 0 or doc is None:
            return [], None, {"status": "unavailable", "returncode": rc, "error": err}
        if i == 0:
            first_doc = doc
            continue
        times.append(elapsed)
        if verbose:
            print(f"      run {i}: {elapsed:.3f}s", file=sys.stderr)
    return times, first_doc, None


def run_cell(
    vmaf_bin: str,
    fixture: Fixture,
    backend: str,
    model_name: str,
    runs: int,
    threads: int,
    verbose: bool,
) -> dict:
    if backend == "cuda":
        free_mib = cuda_free_mib()
        if free_mib < 8000:
            return {
                "status": "not-run",
                "free_mib": free_mib,
                "error": f"below 8000 MiB free ({free_mib} MiB)",
            }
    load_before = load1()
    with tempfile.TemporaryDirectory(prefix="vmaf-bench-") as td:
        out_path = os.path.join(td, "out.json")
        cmd = build_cmd(vmaf_bin, fixture, backend, model_name, out_path, threads)
        times, doc, err_res = run_cell_iterations(cmd, out_path, runs, verbose)
        if err_res is not None or doc is None:
            return err_res or {"status": "unavailable", "error": "unknown error"}

    is_engaged, fallback_reason = check_engagement(doc, backend)
    if not is_engaged:
        return {
            "status": "fallback",
            "backend_requested": backend,
            "backend_used": doc.get("backend_used"),
            "feature_backends": doc.get("feature_backends"),
            "error": fallback_reason or "extractor fallback",
        }

    load_after = load1()
    med = statistics.median(times)
    nframes = len(doc["frames"])
    return {
        "status": "ok",
        "pooled": doc["pooled_metrics"]["vmaf"]["mean"],
        "nframes": nframes,
        "metrics_keys": len(doc["frames"][0]["metrics"]),
        "times": times,
        "median_time": med,
        "best_time": min(times),
        "median_fps": nframes / med,
        "best_fps": nframes / min(times),
        "avg_fps": nframes / statistics.fmean(times),
        "spread_pct": (max(times) - min(times)) / med * 100.0,
        "load_avg_1min": [load_before, load_after],
    }


def parse_args() -> argparse.Namespace:
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
        help="repeatable; default all 16 newest models",
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
    return ap.parse_args()


def load_results(path: str | None) -> dict:
    if path and os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8") as fh:
                return json.load(fh)
        except Exception:
            return {}
    return {}


def save_results(path: str | None, results: dict) -> None:
    if path:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(results, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print(f"\nwrote {path}", flush=True)


def run_dry_run(
    fixtures: list[Fixture],
    models: list[str],
    backends: list[str],
    vmaf_bin: str,
    threads: int,
) -> int:
    for fx in fixtures:
        for mn in models:
            if not can_pair(fx, mn):
                continue
            for be in backends:
                with tempfile.TemporaryDirectory() as td:
                    cmd = build_cmd(
                        vmaf_bin,
                        fx,
                        be,
                        mn,
                        os.path.join(td, "out.json"),
                        threads,
                    )
                print(" ".join(cmd))
    return 0


def report_cell(res: dict) -> None:
    if res["status"] == "ok":
        print(
            f"{res['median_fps']:8.2f} fps  median {res['median_time']:.3f}s  "
            f"spread {res['spread_pct']:.1f}%  pool {res['pooled']:.6f}  "
            f"keys={res['metrics_keys']}  load={res['load_avg_1min'][1]:.1f}",
            flush=True,
        )
    elif res["status"] == "not-run":
        print(f"SKIP (not run, {res['free_mib']} MiB free)", flush=True)
    elif res["status"] == "fallback":
        print(f"FALLBACK ({res['error']})", flush=True)
    else:
        print(f"SKIP ({res['error']})", flush=True)


def execute_benchmarks(
    fixtures: list[Fixture],
    models: list[str],
    backends: list[str],
    args: argparse.Namespace,
    results: dict,
) -> None:
    print(
        f"# vmaf backend baselines — {args.runs} timed runs/cell + 1 warmup, "
        f"threads={args.threads}",
        flush=True,
    )
    print(f"# load average at start: {load1():.2f}", flush=True)
    for fx in fixtures:
        fx_models = [mn for mn in models if can_pair(fx, mn)]
        if not fx_models:
            continue
        if fx.tag not in results:
            results[fx.tag] = {"label": fx.label}
        print(f"\n## {fx.label}", flush=True)
        for mn in fx_models:
            if mn not in results[fx.tag]:
                results[fx.tag][mn] = {}
            print(f"  model={mn}", flush=True)
            for be in backends:
                print(f"    {be:6s} ... ", end="", flush=True)
                res = run_cell(args.vmaf_bin, fx, be, mn, args.runs, args.threads, args.verbose)
                results[fx.tag][mn][be] = res
                report_cell(res)


def resolve_fixtures(args: argparse.Namespace) -> list[Fixture]:
    fixtures = [f for f in FIXTURES if args.fixtures is None or f.tag in args.fixtures]
    missing = [f.tag for f in fixtures if not (os.path.exists(f.ref) and os.path.exists(f.dis))]
    if missing:
        print(f"note: skipping fixtures with absent files: {', '.join(missing)}", file=sys.stderr)
    return [f for f in fixtures if os.path.exists(f.ref) and os.path.exists(f.dis)]


def main() -> int:
    args = parse_args()
    backends = args.backends or ["cpu", "cuda", "sycl", "hip"]
    models = args.models or NEWEST_MODELS

    # Fixture paths are relative to the repository root, the parent of testdata/.
    os.chdir(_REPO_ROOT)

    fixtures = resolve_fixtures(args)
    if not fixtures:
        print("error: no fixture files present", file=sys.stderr)
        return 2

    compatible_pairs = [(f, m) for f in fixtures for m in models if can_pair(f, m)]
    if not compatible_pairs:
        print("error: no compatible fixture and model pairs selected", file=sys.stderr)
        return 2

    if args.dry_run:
        return run_dry_run(fixtures, models, backends, args.vmaf_bin, args.threads)

    results = load_results(args.json_out)
    execute_benchmarks(fixtures, models, backends, args, results)
    save_results(args.json_out, results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
