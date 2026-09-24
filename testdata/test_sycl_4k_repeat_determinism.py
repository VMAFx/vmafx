#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Repeat determinism harness for 4K SYCL on Intel Arc (BUG-040).

Asserts that repeated 4K SYCL runs produce identical scores across
consecutive iterations on the production graph-replay path without
relying on VMAF_SYCL_CHECKSUM or synthetic barriers.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest

TESTDATA_DIR = Path(__file__).resolve().parent
REF_4K = TESTDATA_DIR / "ref_3840x2160_48f.yuv"
DIS_4K = TESTDATA_DIR / "dis_3840x2160_48f.yuv"
CPU_GOLDEN_4K = TESTDATA_DIR / "scores_cpu_4k.json"


def run_sycl_4k_once(iteration: int, tmp_path: Path) -> dict:
    """Run a single 4K SYCL VMAF execution and parse the output metrics."""
    out_json = tmp_path / f"sycl_4k_run_{iteration}.json"
    vmaf_bin = os.environ.get("VMAF_BIN", "/usr/local/bin/vmaf")

    cmd = [
        str(vmaf_bin),
        "-r", str(REF_4K),
        "-d", str(DIS_4K),
        "-w", "3840",
        "-h", "2160",
        "-p", "420",
        "-b", "8",
        "-m", "version=vmaf_v0.6.1",
        "--backend", "sycl",
        "--json",
        "-o", str(out_json),
    ]

    env = os.environ.copy()
    existing_ld = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = f"/usr/local/lib:{existing_ld}".rstrip(":") if existing_ld else "/usr/local/lib"
    if "ONEAPI_DEVICE_SELECTOR" not in env:
        env["ONEAPI_DEVICE_SELECTOR"] = "level_zero:gpu"

    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    assert result.returncode == 0, f"vmaf failed (exit {result.returncode}): {result.stderr[:500]}"
    assert out_json.exists(), f"Output JSON {out_json} not generated"

    with open(out_json, "r", encoding="utf-8") as f:
        return json.load(f)


@pytest.mark.skipif(
    not (REF_4K.exists() and DIS_4K.exists()),
    reason="4K YUV test fixtures not present on host",
)
def test_sycl_4k_consecutive_runs_deterministic(tmp_path: Path) -> None:
    """Run 5 consecutive 4K SYCL runs and verify bit-exact determinism across all runs."""
    num_runs = int(os.environ.get("VMAF_SYCL_REPEAT_COUNT", "5"))
    runs_data = []

    for i in range(num_runs):
        data = run_sycl_4k_once(i, tmp_path)
        vmaf_mean = data["pooled_metrics"]["vmaf"]["mean"]
        adm2_mean = data["pooled_metrics"]["integer_adm2"]["mean"]
        runs_data.append((vmaf_mean, adm2_mean, data))

    # Compare run 0 vs subsequent runs
    ref_vmaf, ref_adm2, ref_full = runs_data[0]
    mismatches = []
    for i, (vmaf_mean, adm2_mean, full_data) in enumerate(runs_data[1:], start=1):
        if abs(vmaf_mean - ref_vmaf) > 1e-6 or abs(adm2_mean - ref_adm2) > 1e-6:
            mismatches.append(
                f"Run {i} deviated from Run 0: vmaf {vmaf_mean:.6f} vs {ref_vmaf:.6f} "
                f"(diff {abs(vmaf_mean - ref_vmaf):.6f}), adm2 {adm2_mean:.6f} vs {ref_adm2:.6f}"
            )

    assert not mismatches, "Nondeterminism detected across 4K SYCL runs:\n" + "\n".join(mismatches)
