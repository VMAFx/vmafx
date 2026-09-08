#!/usr/bin/env python3
# testdata/bench_upstream_ab.py — A/B this fork against upstream Netflix/vmaf.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""A/B the fork's CPU throughput against upstream Netflix/vmaf at a pinned tag.

Every other benchmark in this repo compares the fork against *itself* — one
backend against another, or one commit against a recorded baseline. None of
them answer the question this one exists for: **is the fork actually faster
than the thing it forked, and did it stay exact while getting there?**

Two numbers per cell, and the second one gates the first:

* **speedup** — upstream median wall-clock over fork median wall-clock. Higher
  is better; 1.00 means no gain.
* **score delta** — the pooled VMAF the two binaries emit for the same input.
  This must be zero on the CPU path. A speedup bought by changing the score is
  not a speedup, it is a regression with a nice number attached, so a non-zero
  delta fails the run regardless of timing.

The CPU path is the only honest A/B surface: upstream has no SYCL, HIP or Metal
backend, and its CUDA backend covers a different feature set. Comparing the
fork's GPU throughput against upstream's CPU would measure the hardware, not
the work. `testdata/bench_backends.py` is where per-backend numbers live.

Measurement discipline is inherited from ADR-1185 / `bench_backends.py`, since
a number produced differently is not comparable to the ones already recorded:
one discarded warmup per cell, `--runs` timed repetitions reported as the
median, min/max spread alongside, and the 1-minute load average sampled around
every cell.

Usage:
    testdata/bench_upstream_ab.py --runs 5
    testdata/bench_upstream_ab.py --upstream-ref v3.2.0 --json out.json
    testdata/bench_upstream_ab.py --upstream-bin /path/to/upstream/vmaf
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Upstream's latest release at the time this harness landed. Pinned rather than
# tracking master so a rerun months later is comparable to the recorded table:
# the baseline has to be a fixed point, or the speedup column measures
# upstream's churn as much as the fork's work.
DEFAULT_UPSTREAM_REF = "v3.2.0"
UPSTREAM_URL = "https://github.com/Netflix/vmaf.git"

# Same fixtures as testdata/bench_backends.py. The 4K pair is gitignored and
# fetched separately, so it is included only when present -- but it is the one
# that matters: see MIN_USEFUL_SECONDS below.
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

# Below this, process startup, model parse and JSON emit are a large enough
# share of the wall clock that the speedup column measures them rather than the
# metric kernels. The tracked fixtures are all well under it -- 48 frames of
# 576x324 runs in ~70 ms -- so a run limited to them reports a number close to
# 1.00x no matter what the kernels do. Fetch the 4K pair
# (scripts/test/fetch-test-yuvs.sh) before quoting a speedup as meaningful.
MIN_USEFUL_SECONDS = 2.0

# Only models upstream also ships. The fork's default model (ADR-1169) has no
# upstream counterpart, so asking both binaries for it would compare different
# work. v0.6.1 is present in both trees at the same path.
MODEL = "model/vmaf_v0.6.1.json"

# Both binaries print scores at `%.6f` (upstream has no equivalent of the
# fork's `--precision`, so 6 decimals is the finest comparison the CLI allows),
# which puts the floor of any comparison at 1e-6.
#
# The default ceiling is deliberately above that floor rather than at it. As of
# 2026-09-07 the fork and upstream v3.2.0 agree exactly on all 14 pooled
# features but differ on the final VMAF by up to 8e-6 per frame, in both
# directions, on the checkerboard 1-px pair. That is a genuine open question --
# tracked in docs/state.md -- not a rounding artefact, and it is under the
# Netflix golden gate's `places=4`, which is why it went unnoticed. Until it is
# localised, the gate's job is to catch the delta GROWING, not to fail every
# run on a known quantity.
DEFAULT_MAX_SCORE_DELTA = 1e-5


def load1() -> float:
    with open("/proc/loadavg", encoding="ascii") as fh:
        return float(fh.read().split()[0])


def build_upstream(ref: str, workdir: Path, jobs: int) -> Path:
    """Clone + build upstream at `ref`. Returns the path to its vmaf binary."""
    src = workdir / f"netflix-vmaf-{ref}"
    binary = src / "libvmaf" / "build" / "tools" / "vmaf"
    if binary.exists():
        print(f"==> reusing upstream {ref} build at {binary}", file=sys.stderr)
        return binary

    if not src.exists():
        print(f"==> cloning upstream {ref}", file=sys.stderr)
        subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", ref, UPSTREAM_URL, str(src)],
            check=True,
        )

    # Upstream's build root is libvmaf/, not core/ — the fork moved it in
    # ADR-0700. Build CPU-only: this harness compares the CPU path.
    print(f"==> building upstream {ref} (CPU only)", file=sys.stderr)
    subprocess.run(
        [
            "meson",
            "setup",
            "build",
            "--buildtype=release",
            "-Denable_cuda=false",
            "-Denable_float=true",
        ],
        cwd=src / "libvmaf",
        check=True,
    )
    subprocess.run(["ninja", "-C", "build", "-j", str(jobs)], cwd=src / "libvmaf", check=True)
    if not binary.exists():
        raise SystemExit(f"upstream build produced no binary at {binary}")
    return binary


def build_cmd(vmaf_bin, fixture, model_path, out_path, threads, root):
    _tag, ref, dis, w, h, bd, _label = fixture
    return [
        str(vmaf_bin),
        "--reference",
        str(root / ref),
        "--distorted",
        str(root / dis),
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
        "--model",
        f"path={root / model_path}",
        "--output",
        out_path,
        "--json",
        "-q",
    ]


def run_cell(vmaf_bin, fixture, runs, threads, root, verbose):
    """Time one (binary, fixture) cell. Mirrors bench_backends.py::run_cell."""
    times = []
    pooled = None
    nframes = None
    load_before = load1()

    with tempfile.TemporaryDirectory(prefix="vmaf-ab-") as td:
        out_path = os.path.join(td, "out.json")
        cmd = build_cmd(vmaf_bin, fixture, MODEL, out_path, threads, root)
        # runs + 1: iteration 0 is a discarded warmup (page cache, first-touch
        # allocation), exactly as in bench_backends.py.
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
                continue
            times.append(elapsed)
            if pooled is None:
                with open(out_path, encoding="utf-8") as fh:
                    doc = json.load(fh)
                pooled = doc["pooled_metrics"]["vmaf"]["mean"]
                nframes = len(doc["frames"])
            if verbose:
                print(f"      run {i}: {elapsed:.3f}s", file=sys.stderr)

    med = statistics.median(times)
    return {
        "status": "ok",
        "pooled": pooled,
        "nframes": nframes,
        "times": times,
        "median_time": med,
        "best_time": min(times),
        "median_fps": nframes / med,
        "spread_pct": (max(times) - min(times)) / med * 100.0,
        "load_avg_1min": [load_before, load1()],
    }


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--fork-bin",
        default="core/build/tools/vmaf",
        help="fork vmaf binary (default: core/build/tools/vmaf)",
    )
    ap.add_argument("--upstream-bin", help="prebuilt upstream binary; skips the clone + build")
    ap.add_argument(
        "--upstream-ref",
        default=DEFAULT_UPSTREAM_REF,
        help=f"upstream tag to build (default: {DEFAULT_UPSTREAM_REF})",
    )
    ap.add_argument(
        "--workdir",
        default="build-upstream-ab",
        help="where upstream is cloned and built (default: build-upstream-ab)",
    )
    ap.add_argument("--runs", type=int, default=3, help="timed repetitions per cell")
    ap.add_argument(
        "--threads",
        type=int,
        default=1,
        help="--threads passed to both binaries (default 1: "
        "single-threaded is the comparable surface)",
    )
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument(
        "--max-score-delta",
        type=float,
        default=DEFAULT_MAX_SCORE_DELTA,
        help="fail if |fork - upstream| pooled VMAF exceeds this "
        f"(default {DEFAULT_MAX_SCORE_DELTA:g}; see the module docstring)",
    )
    ap.add_argument("--json", help="write the full result document here")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    root = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True
        ).stdout.strip()
    )
    fork_bin = Path(args.fork_bin)
    if not fork_bin.is_absolute():
        fork_bin = root / fork_bin
    if not fork_bin.exists():
        print(
            f"error: fork binary not found at {fork_bin}\n"
            f"       build it first: meson setup core/build core && ninja -C core/build",
            file=sys.stderr,
        )
        return 2

    present = [f for f in FIXTURES if (root / f[1]).exists() and (root / f[2]).exists()]
    absent = [f[0] for f in FIXTURES if f not in present]
    if not present:
        print(
            "error: no fixtures present\n" "       fetch them: scripts/test/fetch-test-yuvs.sh",
            file=sys.stderr,
        )
        return 2
    if absent:
        print(f"note: skipping absent fixtures: {', '.join(absent)}", file=sys.stderr)

    if args.upstream_bin:
        upstream_bin = Path(args.upstream_bin)
    else:
        workdir = Path(args.workdir)
        if not workdir.is_absolute():
            workdir = root / workdir
        workdir.mkdir(parents=True, exist_ok=True)
        upstream_bin = build_upstream(args.upstream_ref, workdir, args.jobs)

    results = []
    parity_failures = []
    print(
        f"\n{'fixture':<20} {'upstream':>12} {'fork':>12} {'speedup':>9} " f"{'score delta':>12}",
        file=sys.stderr,
    )
    print("-" * 70, file=sys.stderr)

    for fixture in present:
        tag = fixture[0]
        up = run_cell(upstream_bin, fixture, args.runs, args.threads, root, args.verbose)
        fk = run_cell(fork_bin, fixture, args.runs, args.threads, root, args.verbose)
        cell = {"fixture": tag, "label": fixture[6], "upstream": up, "fork": fk}

        if up["status"] == "ok" and fk["status"] == "ok":
            cell["speedup"] = up["median_time"] / fk["median_time"]
            cell["score_delta"] = fk["pooled"] - up["pooled"]
            if abs(cell["score_delta"]) > args.max_score_delta:
                parity_failures.append((tag, up["pooled"], fk["pooled"]))
            print(
                f"{tag:<20} {up['median_time']:>10.3f}s {fk['median_time']:>10.3f}s "
                f"{cell['speedup']:>8.2f}x {cell['score_delta']:>12.2e}",
                file=sys.stderr,
            )
        else:
            bad = up if up["status"] != "ok" else fk
            print(f"{tag:<20} UNAVAILABLE: {bad.get('error')}", file=sys.stderr)
        results.append(cell)

    ok = [c for c in results if "speedup" in c]
    startup_bound = [c for c in ok if c["fork"]["median_time"] < MIN_USEFUL_SECONDS]
    if startup_bound and len(startup_bound) == len(ok):
        print(
            f"\nWARNING: every cell ran in under {MIN_USEFUL_SECONDS:g}s, so process "
            f"startup, model parse and JSON emit dominate the wall clock.\n"
            f"         The speedup column below is close to 1.00x by construction and "
            f"says little about\n         the metric kernels. Fetch the 4K pair "
            f"(scripts/test/fetch-test-yuvs.sh) for a number worth recording.",
            file=sys.stderr,
        )
    doc = {
        "upstream_ref": args.upstream_ref,
        "runs": args.runs,
        "threads": args.threads,
        "cells": results,
        "geomean_speedup": (statistics.geometric_mean([c["speedup"] for c in ok]) if ok else None),
        "score_parity": "FAIL" if parity_failures else "OK",
        "max_score_delta": args.max_score_delta,
        "startup_bound": bool(startup_bound and len(startup_bound) == len(ok)),
    }
    if doc["geomean_speedup"]:
        print(
            f"\ngeomean speedup vs upstream {args.upstream_ref}: " f"{doc['geomean_speedup']:.2f}x",
            file=sys.stderr,
        )

    if args.json:
        Path(args.json).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.json}", file=sys.stderr)

    if parity_failures:
        print(
            "\nSCORE PARITY FAILED — the fork and upstream disagree on the CPU path:",
            file=sys.stderr,
        )
        for tag, u, f in parity_failures:
            print(f"  {tag}: upstream={u!r} fork={f!r} delta={f - u:.3e}", file=sys.stderr)
        print(
            "A speedup that moves the score is a regression. Fix the arithmetic "
            "before recording any timing from this run.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
