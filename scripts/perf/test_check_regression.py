#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for scripts/perf/check-regression.py (ADR-0907)."""

from __future__ import annotations

import importlib.util
import json
from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest

_HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location("check_regression", _HERE / "check-regression.py")
assert _SPEC is not None
check_regression = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(check_regression)


def _baseline_payload() -> dict[str, Any]:
    """Small fabricated baseline covering two backends and two metrics."""
    return {
        "schema_version": "1",
        "runs": [
            {
                "resolution": "576",
                "backend": "cpu",
                "metric": "vif",
                "median_ms": 100,
                "status": "ok",
            },
            {
                "resolution": "576",
                "backend": "cpu",
                "metric": "adm",
                "median_ms": 200,
                "status": "ok",
            },
            {
                "resolution": "576",
                "backend": "cuda",
                "metric": "vif",
                "median_ms": 50,
                "status": "ok",
            },
            {
                "resolution": "576",
                "backend": "cuda",
                "metric": "adm",
                "median_ms": 0,
                "status": "skip",
                "skip_reason": "no GPU",
            },
        ],
    }


def test_identical_runs_pass() -> None:
    baseline = _baseline_payload()
    current = deepcopy(baseline)
    regressions, improvements, skipped = check_regression.compare_runs(
        baseline, current, tolerance_pct=5.0, backends=None
    )
    assert regressions == []
    assert improvements == []
    # One cuda/adm cell is skip in both -> joined-but-skipped.
    assert len(skipped) == 1


def test_regression_above_tolerance_flagged() -> None:
    baseline = _baseline_payload()
    current = deepcopy(baseline)
    # Bump cpu/vif by 10% (above the 5% tolerance).
    current["runs"][0]["median_ms"] = 110
    regressions, _, _ = check_regression.compare_runs(
        baseline, current, tolerance_pct=5.0, backends=None
    )
    assert len(regressions) == 1
    assert regressions[0]["key"] == ("576", "cpu", "vif")
    assert regressions[0]["delta_pct"] == pytest.approx(10.0)


def test_regression_within_tolerance_ignored() -> None:
    baseline = _baseline_payload()
    current = deepcopy(baseline)
    # Bump cpu/vif by 3% (below the 5% tolerance).
    current["runs"][0]["median_ms"] = 103
    regressions, _, _ = check_regression.compare_runs(
        baseline, current, tolerance_pct=5.0, backends=None
    )
    assert regressions == []


def test_improvement_reported_informationally() -> None:
    baseline = _baseline_payload()
    current = deepcopy(baseline)
    # Drop cpu/vif by 20% (well past the negative tolerance).
    current["runs"][0]["median_ms"] = 80
    regressions, improvements, _ = check_regression.compare_runs(
        baseline, current, tolerance_pct=5.0, backends=None
    )
    assert regressions == []
    assert len(improvements) == 1
    assert improvements[0]["key"] == ("576", "cpu", "vif")


def test_backend_filter_restricts_comparison() -> None:
    baseline = _baseline_payload()
    current = deepcopy(baseline)
    # Make cpu/vif regress and cuda/vif also regress. Filter to cpu only.
    current["runs"][0]["median_ms"] = 200
    current["runs"][2]["median_ms"] = 200
    regressions, _, _ = check_regression.compare_runs(
        baseline, current, tolerance_pct=5.0, backends=["cpu"]
    )
    # Only the cpu regression should be visible.
    assert [record["key"] for record in regressions] == [("576", "cpu", "vif")]


def test_missing_current_cell_is_skipped_not_failed() -> None:
    baseline = _baseline_payload()
    current = deepcopy(baseline)
    # Drop cpu/vif from the current run entirely.
    current["runs"] = [cell for cell in current["runs"] if cell["metric"] != "vif"]
    regressions, _, skipped = check_regression.compare_runs(
        baseline, current, tolerance_pct=5.0, backends=None
    )
    assert regressions == []
    skip_keys = {record["key"] for record in skipped}
    assert ("576", "cpu", "vif") in skip_keys
    assert ("576", "cuda", "vif") in skip_keys


def test_real_committed_baseline_loads_cleanly() -> None:
    """The committed baseline JSON must satisfy the gate's reader."""
    baseline_path = Path(__file__).resolve().parents[2] / "testdata" / "perf_multi_resolution.json"
    payload = json.loads(baseline_path.read_text(encoding="utf-8"))
    indexed = check_regression._index_runs(payload)
    assert indexed, "committed baseline produced no indexable cells"


def test_main_exits_nonzero_on_regression(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    baseline = _baseline_payload()
    current = deepcopy(baseline)
    current["runs"][0]["median_ms"] = 200  # 100% regression
    baseline_path = tmp_path / "baseline.json"
    current_path = tmp_path / "current.json"
    baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
    current_path.write_text(json.dumps(current), encoding="utf-8")
    exit_code = check_regression.main(
        ["--baseline", str(baseline_path), "--current", str(current_path)]
    )
    assert exit_code == 1
    captured = capsys.readouterr()
    assert "REGRESSIONS" in captured.out


def test_main_exits_zero_on_clean_run(tmp_path: Path) -> None:
    baseline = _baseline_payload()
    current = deepcopy(baseline)
    baseline_path = tmp_path / "baseline.json"
    current_path = tmp_path / "current.json"
    baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
    current_path.write_text(json.dumps(current), encoding="utf-8")
    exit_code = check_regression.main(
        ["--baseline", str(baseline_path), "--current", str(current_path)]
    )
    assert exit_code == 0
