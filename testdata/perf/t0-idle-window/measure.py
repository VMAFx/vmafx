# -*- coding: utf-8 -*-
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""#3294 idle-window full matrix on the RTX 5080.

Cells: {cpu, cuda} x N in {10, 30, 60, 120}. Four runs per cell: run 0 is the
cold run (kept on disk, discarded from the minimum, per the maintainer's
2026-11-04 guidance), the minimum is taken over runs 1-3. Before each cell a
4-sample utilization gate is recorded (nvidia-smi, GPU 1 = 5080). Every run's
JSON report is kept; per run we extract frames count (asserted == N), fps,
wall_fps, backend_used, feature_backends. OLS fit T(N) = T0 + N*t_frame per
backend on the min-of-3 wall times, primary fit over N>=30, secondary over all
four points. Read-only wrt the repo; artifacts land next to this script.
Usage: python -I measure.py
"""

from __future__ import annotations

import json
import os
import statistics
import subprocess
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE

EXE = r"C:\PLAN\VMAF\verify\vmafx-install\bin\vmafx.exe"
FRAMES_DIR = Path(r"C:\PLAN\VMAF\baseline\t1_frames")
NS = (10, 30, 60, 120)

ENV_BASE = {
    "SYSTEMROOT": os.environ["SYSTEMROOT"],
    "PATH": r"C:\PLAN\VMAF\verify\vmafx-install\bin;C:\msys64\mingw64\bin;"
    + os.environ["SYSTEMROOT"]
    + r"\System32",
    "CUDA_DEVICE_ORDER": "PCI_BUS_ID",
    "CUDA_VISIBLE_DEVICES": "1",
}


def util_sample(k: int = 4) -> list[int]:
    vals = []
    for _ in range(k):
        r = subprocess.run(
            [
                "nvidia-smi",
                "-i",
                "1",
                "--query-gpu=utilization.gpu",
                "--format=csv,noheader,nounits",
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        try:
            vals.append(int(r.stdout.strip()))
        except ValueError:
            vals.append(-1)
        time.sleep(1.0)
    return vals


def run_cell(backend: str, n: int) -> list[dict]:
    ref = FRAMES_DIR / f"ref_1080p8_{n}.yuv"
    dis = FRAMES_DIR / f"dis_1080p8_{n}.yuv"
    runs = []
    for i in range(4):
        report = OUT / f"n{n}-{backend}-{i}.json"
        cmd = [
            EXE,
            "-r",
            str(ref),
            "-d",
            str(dis),
            "-w",
            "1920",
            "-h",
            "1080",
            "-p",
            "420",
            "-b",
            "8",
            "--backend",
            backend,
            "-o",
            str(report),
            "--json",
        ]
        t0 = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True, text=True, env=ENV_BASE, timeout=600)
        wall_ms = (time.perf_counter() - t0) * 1000.0
        if proc.returncode != 0:
            raise RuntimeError(
                f"{backend} n={n} run{i}: EXIT={proc.returncode}\n" f"{proc.stderr[:400]}"
            )
        data = json.loads(report.read_text(encoding="utf-8"))
        frames = data.get("frames", [])
        assert len(frames) == n, f"expected {n} frames, got {len(frames)}"
        # backend_used alone cannot separate a pure run from a mixed one: a
        # CUDA run whose features fell back to CPU must be rejected here, so
        # require a nonempty receipt with every feature on the requested
        # backend before the run counts for the cell.
        receipt = [(f["extractor"], f["backend"]) for f in data.get("feature_backends", [])]
        assert receipt, f"{backend} n={n} run{i}: empty feature_backends receipt"
        offload = [f"{e}->{b}" for e, b in receipt if b != backend]
        assert not offload, (
            f"{backend} n={n} run{i}: features not on the " f"requested backend: {offload}"
        )
        rec = {
            "run": i,
            "wall_ms": round(wall_ms, 1),
            "fps": data.get("fps"),
            "wall_fps": data.get("wall_fps"),
            "backend_used": data.get("backend_used"),
            "feature_backends": receipt,
            "n_frames": len(frames),
        }
        runs.append(rec)
        print(
            f"  {backend} N={n} run{i}: wall={wall_ms:.0f}ms fps={rec['fps']} "
            f"wall_fps={rec['wall_fps']} backend={rec['backend_used']}"
        )
        time.sleep(1.0)
    return runs


def ols(points: list[tuple[float, float]]) -> tuple[float, float, float]:
    """Return (T0, t_frame, R2) for T(N) = T0 + N*t_frame."""
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    n = len(xs)
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    slope = sxy / sxx
    intercept = my - slope * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (intercept + slope * x)) ** 2 for x, y in zip(xs, ys))
    r2 = 1.0 - ss_res / ss_tot if ss_tot else 1.0
    return intercept, slope, r2


def main() -> int:
    gate = util_sample()
    print(f"pre-run util gate (5080, 4 samples): {gate}")
    results = {
        "pre_run_util": gate,
        "cells": {},
        "notes": {
            "machine": "Windows 10.0.26300, RTX 5080 (device 1 via CUDA_VISIBLE_DEVICES)",
            "exe": "rc.3 vmafx.exe (static default model)",
            "input": "1920x1080 yuv420p 8-bit raw (t1_frames)",
            "method": "4 runs/cell: run0 cold discarded, min over runs 1-3; "
            "full-process wall via time.perf_counter",
        },
    }
    # Cells run in an interleaved order (cpu/cuda alternating within each N)
    # so load, thermal and clock drift over the campaign hit both backends
    # alike instead of confounding the backend comparison with position in the
    # run matrix; the executed order is recorded with the raw timings.
    cell_order = [f"{backend}-n{n}" for n in NS for backend in ("cpu", "cuda")]
    results["cell_order"] = cell_order
    results["notes"]["cell_order"] = (
        "cpu and cuda alternate within each N, so campaign drift (load, "
        "thermal, clocks) affects both backends alike; the recorded campaign "
        "ran all cpu cells before any cuda cell (see REPORT.md caveats)"
    )
    for cell in cell_order:
        backend, n = cell.split("-n")
        n = int(n)
        gate = util_sample()
        print(f"cell {backend} N={n} util gate: {gate}")
        runs = run_cell(backend, n)
        results["cells"][cell] = {"util_gate": gate, "runs": runs}
    # fits on min-of-3 (runs 1..3)
    for backend in ("cpu", "cuda"):
        pts = []
        pts_all = []
        for n in NS:
            runs = results["cells"][f"{backend}-n{n}"]["runs"]
            best = min(runs[1:], key=lambda r: r["wall_ms"])
            pts_all.append((n, best["wall_ms"]))
            if n >= 30:
                pts.append((n, best["wall_ms"]))
        t0, slope, r2 = ols(pts)
        t0a, slopea, r2a = ols(pts_all)
        results[f"fit_{backend}"] = {
            "primary_Nge30": {
                "T0_ms": round(t0, 1),
                "t_frame_ms": round(slope, 3),
                "R2": round(r2, 4),
            },
            "all_points": {
                "T0_ms": round(t0a, 1),
                "t_frame_ms": round(slopea, 3),
                "R2": round(r2a, 4),
            },
        }
        print(
            f"fit {backend}: N>=30 T0={t0:.1f}ms t={slope:.3f}ms R2={r2:.4f} | "
            f"all T0={t0a:.1f}ms t={slopea:.3f}ms R2={r2a:.4f}"
        )
    (OUT / "summary.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print("saved", OUT / "summary.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
