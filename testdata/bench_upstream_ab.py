#!/usr/bin/env python3
# testdata/bench_upstream_ab.py — A/B this fork against upstream Netflix/vmaf.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A/B the fork's CPU throughput against upstream Netflix/vmaf at a pinned commit.

Every other benchmark in this repo compares the fork against *itself* — one
backend against another, or one commit against a recorded baseline. None of
them answer the question this one exists for: **is the fork actually faster
than the thing it forked, and did it stay exact while getting there?**

Two results per cell, and the second one gates the first:

* **speedup** — upstream median wall-clock over fork median wall-clock. Higher
  is better; 1.00 means no gain.
* **score parity** — the upstream parity guard's comparison
  (`scripts/dev/upstream_parity.py`, ADR-1487) of the model on the same
  fixtures: every value both trees emit at `%.17g`, against the allowlist of
  recorded deviations. A speedup bought by changing the score is not a
  speedup, it is a regression with a nice number attached, so a difference
  the allowlist does not cover fails the run regardless of timing.

Upstream is built at the commit the repository records as the head it is at
parity with (`docs/development/known-upstream-bugs.md`), by the guard's own
build step; `--upstream-ref` names another commit or tag. The fork side is the
golden-profile build the guard uses (in the guard's work directory), so the
binary that is timed is the one whose scores are checked. The guard's verdict
is evidence only in the pinned environment (the dev container image); a bench
run on the host reports it as advisory.

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
    testdata/bench_upstream_ab.py --upstream-bin /path/to/upstream/vmaf   # timing only
"""

import argparse
import json
import os
import statistics
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from scripts.ci import upstream_parity_allowlist  # noqa: E402
from scripts.dev import upstream_parity as parity  # noqa: E402
from scripts.dev import upstream_parity_matrix as parity_matrix  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

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
        "nflx8",
    ),
    (
        "checkerboard_1px",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv",
        1920,
        1080,
        8,
        "Checkerboard 1-px shift, 1920x1080, 3f",
        "cb1",
    ),
    (
        "checkerboard_10px",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv",
        "python/test/resource/yuv/checkerboard_1920_1080_10_3_10_0.yuv",
        1920,
        1080,
        8,
        "Checkerboard 10-px shift, 1920x1080, 3f",
        "cb10",
    ),
    (
        "bbb_4k_200f",
        "testdata/bbb/ref_3840x2160_200f.yuv",
        "testdata/bbb/dis_3840x2160_200f.yuv",
        3840,
        2160,
        8,
        "BBB 4K, 3840x2160, 200f",
        "bbb4k",
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

# The model run the parity guard compares for the bench's fixtures: the guard's
# name for it and the harness request ({model} is each tree's model directory).
PARITY_RUN = ("M.vmaf_v0.6.1", "M:{model}/vmaf_v0.6.1.json")

EXIT_OK = 0
EXIT_PARITY = 1
EXIT_USAGE = 2


def load1() -> float:
    with open("/proc/loadavg", encoding="ascii") as fh:
        return float(fh.read().split()[0])


def score_parity(trees, fixtures, workdir: Path, envdir: Path, jobs: int):
    """The guard's verdict on the model for *fixtures*: (status, report lines).

    One model run per fixture at the default dispatch, the same comparison and
    the same allowlist as `make upstream-parity`. The run set is a slice of
    the guard's matrix, so a fragment without a difference here is not judged
    stale. Outside the pinned environment the verdict is marked advisory.
    """

    upstream_tree, fork_tree = trees
    names = [fixture[7] for fixture in fixtures]
    runs = [parity_matrix.Run(name, *PARITY_RUN) for name in names]
    fixtures_dir = workdir / "fixtures"
    usable, skipped = parity.available_fixtures(names, fixtures_dir)
    runs = [run for run in runs if run.fixture in usable]
    documents = [
        parity.run_tree(tree, runs, ("default",), fixtures_dir, envdir, jobs)
        for tree in (upstream_tree, fork_tree)
    ]
    fragments = upstream_parity_allowlist.load_fragments()
    return parity.judge(*documents, fragments, skipped, complete=False, allow_unpinned=True)


def build_cmd(vmaf_bin, fixture, model_path, out_path, threads, root):
    _tag, ref, dis, w, h, bd, _label, _parity_name = fixture
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


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--fork-build",
        help="golden-profile build directory of this tree: timed binary and parity harness "
        "(default: fork-golden in the guard's directory for this environment)",
    )
    ap.add_argument("--fork-bin", help="time this fork binary instead of <fork-build>/tools/vmaf")
    ap.add_argument(
        "--upstream-bin",
        help="prebuilt upstream binary; skips the upstream build, and with it the score "
        "parity check (reported as NOT RUN)",
    )
    ap.add_argument(
        "--upstream-ref",
        help="Netflix/vmaf commit or tag to build (default: the recorded parity head)",
    )
    ap.add_argument(
        "--workdir",
        default=parity.DEFAULT_WORKDIR,
        help=f"where upstream is exported and built (default: {parity.DEFAULT_WORKDIR})",
    )
    ap.add_argument("--runs", type=int, default=3, help="timed repetitions per cell")
    ap.add_argument(
        "--threads",
        type=int,
        default=1,
        help="--threads passed to both binaries (default 1: "
        "single-threaded is the comparable surface)",
    )
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    ap.add_argument("--json", help="write the full result document here")
    ap.add_argument("-v", "--verbose", action="store_true")
    return ap.parse_args()


def fork_build_dir(args: argparse.Namespace, envdir: Path) -> Path:
    """The golden-profile build of this tree the bench times and checks."""

    return (
        (REPO / args.fork_build).resolve() if args.fork_build else parity.default_fork_build(envdir)
    )


def build_trees(args: argparse.Namespace, envdir: Path, image: str):
    """Build both sides with the guard's build steps: (upstream tree, fork tree)."""

    commit = parity.resolve_ref(args.upstream_ref) if args.upstream_ref else parity.resolve_pin()
    return parity.build_trees(commit, fork_build_dir(args, envdir), envdir, args.jobs, image)


def time_fixtures(args, binaries, present):
    """Time every present fixture on both binaries; print and return the cells."""

    upstream_bin, fork_bin = binaries
    results = []
    print(
        f"\n{'fixture':<20} {'upstream':>12} {'fork':>12} {'speedup':>9} {'pooled delta':>13}",
        file=sys.stderr,
    )
    print("-" * 70, file=sys.stderr)
    for fixture in present:
        tag = fixture[0]
        up = run_cell(upstream_bin, fixture, args.runs, args.threads, REPO, args.verbose)
        fk = run_cell(fork_bin, fixture, args.runs, args.threads, REPO, args.verbose)
        cell = {"fixture": tag, "label": fixture[6], "upstream": up, "fork": fk}
        if up["status"] == "ok" and fk["status"] == "ok":
            cell["speedup"] = up["median_time"] / fk["median_time"]
            # Informational: the command-line tools print six decimals. The
            # verdict is score_parity(), which compares at %.17g.
            cell["score_delta"] = fk["pooled"] - up["pooled"]
            print(
                f"{tag:<20} {up['median_time']:>10.3f}s {fk['median_time']:>10.3f}s "
                f"{cell['speedup']:>8.2f}x {cell['score_delta']:>13.2e}",
                file=sys.stderr,
            )
        else:
            bad = up if up["status"] != "ok" else fk
            print(f"{tag:<20} UNAVAILABLE: {bad.get('error')}", file=sys.stderr)
        results.append(cell)
    return results


def warn_if_startup_bound(ok) -> bool:
    startup_bound = [c for c in ok if c["fork"]["median_time"] < MIN_USEFUL_SECONDS]
    if not (startup_bound and len(startup_bound) == len(ok)):
        return False
    print(
        f"\nWARNING: every cell ran in under {MIN_USEFUL_SECONDS:g}s, so process "
        f"startup, model parse and JSON emit dominate the wall clock.\n"
        f"         The speedup column below is close to 1.00x by construction and "
        f"says little about\n         the metric kernels. Fetch the 4K pair "
        f"(scripts/test/fetch-test-yuvs.sh) for a number worth recording.",
        file=sys.stderr,
    )
    return True


def present_fixtures():
    present = [f for f in FIXTURES if (REPO / f[1]).exists() and (REPO / f[2]).exists()]
    absent = [f[0] for f in FIXTURES if f not in present]
    if absent and present:
        print(f"note: skipping absent fixtures: {', '.join(absent)}", file=sys.stderr)
    return present


def main() -> int:
    args = parse_args()
    present = present_fixtures()
    if not present:
        print(
            "error: no fixtures present\n       fetch them: scripts/test/fetch-test-yuvs.sh",
            file=sys.stderr,
        )
        return EXIT_USAGE
    workdir = (REPO / args.workdir).resolve()
    workdir.mkdir(parents=True, exist_ok=True)
    image = parity.container_image()
    envdir = parity.environment_dir(workdir, image)

    parity_status, parity_lines = None, ["score parity: NOT RUN (prebuilt upstream binary)"]
    try:
        if args.upstream_bin:
            upstream_bin = Path(args.upstream_bin)
            fork_default = fork_build_dir(args, envdir) / "tools" / "vmaf"
            upstream_ref = args.upstream_ref or "prebuilt"
        else:
            trees = build_trees(args, envdir, image)
            upstream_bin, fork_default = trees[0].vmaf, trees[1].vmaf
            upstream_ref = trees[0].commit
            parity_status, parity_lines = score_parity(trees, present, workdir, envdir, args.jobs)
    except (parity.CannotRun, upstream_parity_allowlist.AllowlistError) as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_USAGE
    fork_bin = Path(args.fork_bin).resolve() if args.fork_bin else fork_default
    if not fork_bin.exists():
        print(f"error: fork binary not found at {fork_bin}", file=sys.stderr)
        return EXIT_USAGE

    results = time_fixtures(args, (upstream_bin, fork_bin), present)
    ok = [c for c in results if "speedup" in c]
    verdict = {None: "NOT RUN", parity.EXIT_PASS: "OK"}.get(parity_status, "FAIL")
    doc = {
        "upstream_ref": upstream_ref,
        "runs": args.runs,
        "threads": args.threads,
        "cells": results,
        "geomean_speedup": (statistics.geometric_mean([c["speedup"] for c in ok]) if ok else None),
        "score_parity": verdict,
        "score_parity_report": parity_lines,
        "startup_bound": warn_if_startup_bound(ok),
    }
    if doc["geomean_speedup"]:
        print(
            f"\ngeomean speedup vs upstream {upstream_ref[:12]}: {doc['geomean_speedup']:.2f}x",
            file=sys.stderr,
        )
    print("\n".join(parity_lines), file=sys.stderr)
    if args.json:
        Path(args.json).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.json}", file=sys.stderr)

    if verdict == "FAIL":
        print(
            "\nSCORE PARITY FAILED — the fork and upstream differ on the CPU path where no "
            "recorded deviation covers it.\nA speedup that moves the score is a regression. "
            "Fix the arithmetic, or record the deviation by ADR and fragment "
            "(docs/development/upstream-parity.md), before recording any timing from this run.",
            file=sys.stderr,
        )
        return EXIT_PARITY
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
