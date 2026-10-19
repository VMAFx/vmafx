#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit and mutation tests for tiny_ai_cross_device_parity_gate.py (T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25).

Covers:
1. Missing provider fail-closed behavior (and --allow-missing-provider opt-in)
2. Numerical mismatch beyond tolerance (> 1e-4 FP32, > 1e-2 FP16)
3. Numerical match within tolerance (<= 1e-4 FP32, <= 1e-2 FP16)
4. Target provider silent fallback / unbound detection
5. End-to-end CLI execution with real repository models and fixtures
6. Output artifact schema (JSON and Markdown)
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.tiny_ai_cross_device_parity_gate import (
    DEFAULT_FP16_TOLERANCE,
    DEFAULT_FP32_TOLERANCE,
    evaluate_case,
    load_fp16_inputs,
    load_fp32_inputs,
    main,
    render_json,
    render_markdown,
    run_parity_gate,
)

EXPECTED_CASES = 2
REPO_ROOT = Path(__file__).resolve().parents[2]
FP32_MODEL = REPO_ROOT / "model" / "tiny" / "vmaf_tiny_v2.onnx"
FP16_MODEL = REPO_ROOT / "model" / "tiny" / "smoke_fp16_v0.onnx"
FEATURES_PARQUET = REPO_ROOT / "ai" / "testdata" / "bisect" / "features.parquet"


def test_missing_target_provider_fails_closed() -> None:
    """Missing target provider must report MISSING_PROVIDER and ok=False."""
    report = run_parity_gate(
        fp32_model=FP32_MODEL,
        fp16_model=FP16_MODEL,
        features_path=FEATURES_PARQUET,
        ref_provider="CPUExecutionProvider",
        target_provider="NonExistentExecutionProvider",
        allow_missing_provider=False,
    )
    assert not report.ok
    assert report.status == "MISSING_PROVIDER"
    assert not report.target_available
    assert "NonExistentExecutionProvider" in report.note


def test_missing_target_provider_allow_flag() -> None:
    """With allow_missing_provider=True, missing provider yields ok=True."""
    report = run_parity_gate(
        fp32_model=FP32_MODEL,
        fp16_model=FP16_MODEL,
        features_path=FEATURES_PARQUET,
        ref_provider="CPUExecutionProvider",
        target_provider="NonExistentExecutionProvider",
        allow_missing_provider=True,
    )
    assert report.ok
    assert report.status == "MISSING_PROVIDER"
    assert not report.target_available


def test_missing_reference_provider_fails_closed() -> None:
    """Missing reference provider must fail closed."""
    report = run_parity_gate(
        fp32_model=FP32_MODEL,
        fp16_model=FP16_MODEL,
        features_path=FEATURES_PARQUET,
        ref_provider="MissingRefProvider",
        target_provider="CPUExecutionProvider",
    )
    assert not report.ok
    assert report.status == "MISSING_REF_PROVIDER"


def test_target_provider_unbound_detection(tmp_path: Path) -> None:
    """If target session falls back and does not bind target provider, fail closed."""
    mock_ref_sess = MagicMock()
    mock_ref_sess.get_providers.return_value = ["CPUExecutionProvider"]
    mock_ref_sess.get_inputs.return_value = [MagicMock(name="features")]

    mock_target_sess = MagicMock()
    # Simulate ORT fallback: requested CUDA, but session bound CPU only
    mock_target_sess.get_providers.return_value = ["CPUExecutionProvider"]

    with patch(
        "scripts.ci.tiny_ai_cross_device_parity_gate.create_session",
        side_effect=[mock_ref_sess, mock_target_sess],
    ):
        case = evaluate_case(
            name="FP32 test",
            model_path=FP32_MODEL,
            precision="fp32",
            tolerance=1e-4,
            input_data=np.zeros((10, 6), dtype=np.float32),
            ref_provider="CPUExecutionProvider",
            target_provider="CUDAExecutionProvider",
        )
        assert not case.ok
        assert case.status == "PROVIDER_UNBOUND"
        assert "CUDAExecutionProvider" in case.note


def test_numerical_mismatch_fp32_fails_closed() -> None:
    """Error exceeding FP32 tolerance (1e-4) must fail closed."""
    mock_ref_sess = MagicMock()
    mock_ref_sess.get_providers.return_value = ["CPUExecutionProvider"]
    mock_ref_sess.get_inputs.return_value = [MagicMock(name="features")]

    mock_target_sess = MagicMock()
    mock_target_sess.get_providers.return_value = ["TargetProvider"]
    mock_target_sess.get_inputs.return_value = [MagicMock(name="features")]

    ref_output = np.array([50.0, 60.0, 70.0], dtype=np.float32)
    # Delta of 5e-4 exceeds tolerance of 1e-4
    target_output = np.array([50.0005, 60.0, 70.0], dtype=np.float32)

    with (
        patch(
            "scripts.ci.tiny_ai_cross_device_parity_gate.create_session",
            side_effect=[mock_ref_sess, mock_target_sess],
        ),
        patch(
            "scripts.ci.tiny_ai_cross_device_parity_gate.run_session_inference",
            side_effect=[ref_output, target_output],
        ),
    ):
        case = evaluate_case(
            name="FP32 test",
            model_path=FP32_MODEL,
            precision="fp32",
            tolerance=DEFAULT_FP32_TOLERANCE,
            input_data=np.zeros((3, 6), dtype=np.float32),
            ref_provider="CPUExecutionProvider",
            target_provider="TargetProvider",
        )
        assert not case.ok
        assert case.status == "FAIL"
        assert case.max_abs_error > DEFAULT_FP32_TOLERANCE
        assert "exceeds tolerance" in case.note


def test_numerical_mismatch_fp16_fails_closed() -> None:
    """Error exceeding FP16 tolerance (1e-2) must fail closed."""
    mock_ref_sess = MagicMock()
    mock_ref_sess.get_providers.return_value = ["CPUExecutionProvider"]
    mock_ref_sess.get_inputs.return_value = [MagicMock(name="x")]

    mock_target_sess = MagicMock()
    mock_target_sess.get_providers.return_value = ["TargetProvider"]
    mock_target_sess.get_inputs.return_value = [MagicMock(name="x")]

    ref_output = np.array([[[[0.5, 0.5], [0.5, 0.5]]]], dtype=np.float16)
    # Delta of 0.05 exceeds tolerance of 0.01
    target_output = np.array([[[[0.55, 0.5], [0.5, 0.5]]]], dtype=np.float16)

    with (
        patch(
            "scripts.ci.tiny_ai_cross_device_parity_gate.create_session",
            side_effect=[mock_ref_sess, mock_target_sess],
        ),
        patch(
            "scripts.ci.tiny_ai_cross_device_parity_gate.run_session_inference",
            side_effect=[ref_output, target_output],
        ),
    ):
        case = evaluate_case(
            name="FP16 test",
            model_path=FP16_MODEL,
            precision="fp16",
            tolerance=DEFAULT_FP16_TOLERANCE,
            input_data=np.zeros((1, 1, 2, 2), dtype=np.float16),
            ref_provider="CPUExecutionProvider",
            target_provider="TargetProvider",
        )
        assert not case.ok
        assert case.status == "FAIL"
        assert case.max_abs_error > DEFAULT_FP16_TOLERANCE


def test_numerical_match_within_tolerance_passes() -> None:
    """Error within tolerance must report status OK and ok=True."""
    mock_ref_sess = MagicMock()
    mock_ref_sess.get_providers.return_value = ["CPUExecutionProvider"]
    mock_ref_sess.get_inputs.return_value = [MagicMock(name="features")]

    mock_target_sess = MagicMock()
    mock_target_sess.get_providers.return_value = ["TargetProvider"]
    mock_target_sess.get_inputs.return_value = [MagicMock(name="features")]

    ref_output = np.array([50.0, 60.0, 70.0], dtype=np.float32)
    # Delta of 5e-5 is within tolerance of 1e-4
    target_output = np.array([50.00005, 60.0, 70.0], dtype=np.float32)

    with (
        patch(
            "scripts.ci.tiny_ai_cross_device_parity_gate.create_session",
            side_effect=[mock_ref_sess, mock_target_sess],
        ),
        patch(
            "scripts.ci.tiny_ai_cross_device_parity_gate.run_session_inference",
            side_effect=[ref_output, target_output],
        ),
    ):
        case = evaluate_case(
            name="FP32 test",
            model_path=FP32_MODEL,
            precision="fp32",
            tolerance=DEFAULT_FP32_TOLERANCE,
            input_data=np.zeros((3, 6), dtype=np.float32),
            ref_provider="CPUExecutionProvider",
            target_provider="TargetProvider",
        )
        assert case.ok
        assert case.status == "OK"
        assert case.max_abs_error <= DEFAULT_FP32_TOLERANCE


def test_missing_model_file_fails(tmp_path: Path) -> None:
    """Missing model path must return ERROR."""
    non_existent = tmp_path / "does_not_exist.onnx"
    case = evaluate_case(
        name="Missing model",
        model_path=non_existent,
        precision="fp32",
        tolerance=1e-4,
        input_data=np.zeros((1, 6), dtype=np.float32),
        ref_provider="CPUExecutionProvider",
        target_provider="CPUExecutionProvider",
    )
    assert not case.ok
    assert case.status == "ERROR"
    assert "not found" in case.note


def test_real_models_cpu_self_parity() -> None:
    """Real in-tree models comparing CPU vs CPU must produce zero error and PASS."""
    report = run_parity_gate(
        fp32_model=FP32_MODEL,
        fp16_model=FP16_MODEL,
        features_path=FEATURES_PARQUET,
        ref_provider="CPUExecutionProvider",
        target_provider="CPUExecutionProvider",
        fp32_tol=DEFAULT_FP32_TOLERANCE,
        fp16_tol=DEFAULT_FP16_TOLERANCE,
    )
    assert report.ok
    assert report.status == "PASS"
    assert len(report.cases) == EXPECTED_CASES
    for c in report.cases:
        assert c.ok
        assert c.status == "OK"
        assert c.max_abs_error == 0.0


def test_load_fp32_inputs_real_parquet() -> None:
    """Real parquet loader extracts 6 canonical columns with float32 type."""
    arr = load_fp32_inputs(FEATURES_PARQUET, max_rows=10)
    assert arr.shape == (10, 6)
    assert arr.dtype == np.float32


def test_missing_feature_fixture_is_an_error_not_a_substitute(tmp_path: Path) -> None:
    """No invented inputs: a missing fixture raises, and the gate reports BAD_FIXTURE."""
    import pytest

    with pytest.raises(FileNotFoundError, match=r"absent\.parquet"):
        load_fp32_inputs(tmp_path / "absent.parquet", max_rows=5)
    report = run_parity_gate(
        fp32_model=FP32_MODEL,
        fp16_model=FP16_MODEL,
        features_path=tmp_path / "absent.parquet",
        ref_provider="CPUExecutionProvider",
        target_provider="CPUExecutionProvider",
    )
    assert not report.ok and report.status == "BAD_FIXTURE"
    assert "absent.parquet" in report.note


def test_load_fp16_inputs_deterministic() -> None:
    """FP16 tensor is deterministic float16 shape [1, 1, 2, 2]."""
    t1 = load_fp16_inputs()
    t2 = load_fp16_inputs()
    assert np.array_equal(t1, t2)
    assert t1.dtype == np.float16
    assert t1.shape == (1, 1, 2, 2)


def test_report_serialization_and_markdown(tmp_path: Path) -> None:
    """Report serializes to valid JSON and human-readable Markdown table."""
    report = run_parity_gate(
        fp32_model=FP32_MODEL,
        fp16_model=FP16_MODEL,
        features_path=FEATURES_PARQUET,
        ref_provider="CPUExecutionProvider",
        target_provider="CPUExecutionProvider",
    )
    json_dict = render_json(report)
    assert json_dict["schema_version"] == 1
    assert json_dict["ok"] is True
    assert json_dict["status"] == "PASS"
    assert len(json_dict["cases"]) == EXPECTED_CASES

    md_text = render_markdown(report)
    assert (
        "# Tiny AI Cross-Device Parity Gate Report (T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25)"
        in md_text
    )
    assert "FP32 vmaf_tiny_v2" in md_text
    assert "FP16 smoke_fp16_v0" in md_text
    assert "**PASS**" in md_text


def test_cli_main_success_and_file_writes(tmp_path: Path) -> None:
    """CLI exits 0 on matching providers and writes json/md artifacts."""
    json_path = tmp_path / "report.json"
    md_path = tmp_path / "report.md"

    rc = main(
        [
            "--target-provider",
            "CPUExecutionProvider",
            "--json-out",
            str(json_path),
            "--md-out",
            str(md_path),
        ]
    )
    assert rc == 0
    assert json_path.is_file()
    assert md_path.is_file()

    payload = json.loads(json_path.read_text(encoding="utf-8"))
    assert payload["ok"] is True
    assert payload["status"] == "PASS"


def test_cli_main_missing_provider_fails(tmp_path: Path) -> None:
    """CLI exits 1 on missing target provider without allow flag."""
    rc = main(
        [
            "--target-provider",
            "UnavailableProvider",
        ]
    )
    assert rc == 1
