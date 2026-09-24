#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Repeat determinism harness for 4K SYCL on Intel Arc (BUG-040).

Asserts that 20 repeated 4K SYCL runs produce identical normalized reports
on both the serial and ``--threads 1`` production graph-replay paths without
relying on VMAF_SYCL_CHECKSUM or synthetic barriers.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest

from testdata.run_sycl_scores import build_vmaf_cmd, prepare_sycl_env

TESTDATA_DIR = Path(__file__).resolve().parent
REF_4K = TESTDATA_DIR / "ref_3840x2160_48f.yuv"
DIS_4K = TESTDATA_DIR / "dis_3840x2160_48f.yuv"
REGRESSION_OVERRIDE_ENV_VARS = (
    "VMAF_SYCL_CHECKSUM",
    "VMAF_SYCL_DISPATCH",
    "VMAF_SYCL_USE_GRAPH",
    "VMAF_SYCL_NO_GRAPH",
)


def normalize_score_report(report: dict) -> dict:
    """Remove execution-rate metadata while retaining every scored value."""
    return {key: value for key, value in report.items() if key != "fps"}


def run_sycl_4k_once(iteration: int, tmp_path: Path, threads: int | None) -> dict:
    """Run a single 4K SYCL VMAF execution and parse the output metrics."""
    out_json = tmp_path / f"sycl_4k_run_{iteration}.json"
    vmaf_bin = os.environ.get("VMAF_BIN", "/usr/local/bin/vmaf")

    cmd = build_vmaf_cmd(
        vmaf_bin=vmaf_bin,
        ref=REF_4K,
        dis=DIS_4K,
        width=3840,
        height=2160,
        model="version=vmaf_v0.6.1",
        output_path=out_json,
        backend="sycl",
    )
    if threads is not None:
        cmd.extend(["--threads", str(threads)])

    env = prepare_sycl_env(vmaf_bin)
    for name in REGRESSION_OVERRIDE_ENV_VARS:
        env.pop(name, None)

    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    assert result.returncode == 0, f"vmaf failed (exit {result.returncode}): {result.stderr[:500]}"
    assert out_json.exists(), f"Output JSON {out_json} not generated"

    with open(out_json, "r", encoding="utf-8") as f:
        return json.load(f)


def test_run_sycl_4k_once_removes_diagnostic_and_dispatch_overrides(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Keep the regression on the production graph path despite caller overrides."""
    override_names = (
        "VMAF_SYCL_CHECKSUM",
        "VMAF_SYCL_DISPATCH",
        "VMAF_SYCL_USE_GRAPH",
        "VMAF_SYCL_NO_GRAPH",
    )
    for name in override_names:
        monkeypatch.setenv(name, "forced-by-caller")

    captured_env: dict[str, str] = {}

    def fake_run(
        cmd: list[str], *, capture_output: bool, text: bool, env: dict[str, str]
    ) -> subprocess.CompletedProcess[str]:
        del capture_output, text
        captured_env.update(env)
        output_path = Path(cmd[cmd.index("-o") + 1])
        output_path.write_text('{"version": "test"}', encoding="utf-8")
        return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

    monkeypatch.setattr(subprocess, "run", fake_run)

    report = run_sycl_4k_once(0, tmp_path, threads=None)

    assert report == {"version": "test"}
    assert all(name not in captured_env for name in override_names)


@pytest.mark.skipif(
    not (REF_4K.exists() and DIS_4K.exists()),
    reason="4K YUV test fixtures not present on host",
)
@pytest.mark.parametrize("threads", [None, 1], ids=["serial", "threads-1"])
def test_sycl_4k_consecutive_runs_deterministic(tmp_path: Path, threads: int | None) -> None:
    """Verify 20 full normalized reports are bit-exact in serial and threaded modes."""
    num_runs = int(os.environ.get("VMAF_SYCL_REPEAT_COUNT", "20"))
    assert num_runs >= 2, "VMAF_SYCL_REPEAT_COUNT must be at least 2"
    reports = []

    for i in range(num_runs):
        reports.append(normalize_score_report(run_sycl_4k_once(i, tmp_path, threads)))

    for i, report in enumerate(reports[1:], start=1):
        assert report == reports[0], (
            f"4K SYCL normalized report changed on run {i} "
            f"(threads={threads!r}); all frame, pooled, aggregate, and backend values must match"
        )
