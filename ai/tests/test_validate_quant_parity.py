# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for ai/scripts/validate_quant_parity.py (Research-2029 §6 / #1242)."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("onnxruntime")
pytest.importorskip("pandas")

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / "ai" / "scripts" / "validate_quant_parity.py"
SCORES_JSON = REPO_ROOT / "testdata" / "scores_cpu_576.json"
BISECT_PARQUET = REPO_ROOT / "ai" / "testdata" / "bisect" / "features.parquet"

if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from ai.scripts.validate_quant_parity import (  # noqa: E402
    ParityThresholds,
    _evaluate_model,
    _load_features,
)


def test_load_features_json() -> None:
    features, gt = _load_features(SCORES_JSON)
    assert features.ndim == 2
    assert features.shape == (48, 6)
    assert gt is not None
    assert len(gt) == 48


def test_load_features_parquet() -> None:
    features, gt = _load_features(BISECT_PARQUET)
    assert features.ndim == 2
    assert features.shape == (256, 6)
    assert gt is not None
    assert len(gt) == 256


def test_evaluate_model_defaults_and_relaxed() -> None:
    fp32 = REPO_ROOT / "model" / "tiny" / "vmaf_tiny_v3.onnx"
    int8 = REPO_ROOT / "model" / "tiny" / "vmaf_tiny_v3.int8.onnx"
    features, gt = _load_features(SCORES_JSON)

    # Defaults: strict Research-2029 §6 thresholds fail on un-retrained dynamic PTQ
    res_default = _evaluate_model(fp32, int8, features, "vmaf_tiny_v3", ParityThresholds(), gt)
    assert res_default.plcc >= 0.990
    assert res_default.pass_plcc is True
    assert res_default.pass_mean is False
    assert res_default.pass_max is False
    assert res_default.ok is False

    # Relaxed thresholds pass
    res_relaxed = _evaluate_model(
        fp32,
        int8,
        features,
        "vmaf_tiny_v3",
        ParityThresholds(max_mean_delta=1.0, max_single_delta=1.5),
        gt,
    )
    assert res_relaxed.ok is True


def test_cli_default_run_fails_with_exit_1() -> None:
    cmd = [
        sys.executable,
        str(SCRIPT_PATH),
        "--model",
        "vmaf_tiny_v3",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO_ROOT)
    assert proc.returncode == 1, f"Expected exit code 1, got {proc.returncode}"
    assert "FAIL" in proc.stdout
    assert "Retraining under QAT (Epic #1246) is required" in proc.stdout


def test_cli_relaxed_run_succeeds_with_exit_0() -> None:
    cmd = [
        sys.executable,
        str(SCRIPT_PATH),
        "--model",
        "vmaf_tiny_v3",
        "--max-mean-delta",
        "1.0",
        "--max-single-delta",
        "1.5",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO_ROOT)
    assert proc.returncode == 0, f"Expected exit code 0, got {proc.returncode}\n{proc.stderr}"
    assert "Overall Parity Gate: PASS" in proc.stdout


def test_cli_manifest_output(tmp_path: Path) -> None:
    out_json = tmp_path / "parity_report.json"
    cmd = [
        sys.executable,
        str(SCRIPT_PATH),
        "--model",
        "vmaf_tiny_v4",
        "--out-json",
        str(out_json),
        "--max-mean-delta",
        "1.0",
        "--max-single-delta",
        "1.5",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO_ROOT)
    assert proc.returncode == 0
    assert out_json.is_file()
    data = json.loads(out_json.read_text())
    assert data["gate_pass"] is True
    assert data["n_frames"] == 48
    assert len(data["models"]) == 1
    assert data["models"][0]["model_id"] == "vmaf_tiny_v4"
    assert data["run_provenance"]["schema"] == "ai-run-provenance-v1"


def test_cli_all_flag() -> None:
    cmd = [
        sys.executable,
        str(SCRIPT_PATH),
        "--all",
        "--max-mean-delta",
        "2.0",
        "--max-single-delta",
        "3.0",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO_ROOT)
    assert proc.returncode == 0, f"Expected exit code 0, got {proc.returncode}\n{proc.stderr}"
    assert "vmaf_tiny_v3" in proc.stdout
    assert "vmaf_tiny_v4" in proc.stdout
    assert "Overall Parity Gate: PASS" in proc.stdout


def test_cli_fp32_int8_override() -> None:
    fp32 = REPO_ROOT / "model" / "tiny" / "vmaf_tiny_v4.onnx"
    int8 = REPO_ROOT / "model" / "tiny" / "vmaf_tiny_v4.int8.onnx"
    cmd = [
        sys.executable,
        str(SCRIPT_PATH),
        "--fp32",
        str(fp32),
        "--int8",
        str(int8),
        "--id",
        "custom_v4_test",
        "--max-mean-delta",
        "1.0",
        "--max-single-delta",
        "1.5",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO_ROOT)
    assert proc.returncode == 0, f"Expected exit code 0, got {proc.returncode}\n{proc.stderr}"
    assert "Model: custom_v4_test" in proc.stdout
