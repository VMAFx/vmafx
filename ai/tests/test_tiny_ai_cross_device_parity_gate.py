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
7. The CUDA use_tf32=0 provider-option pin on every session
8. The transnet_v2 provider-gap probe (named ScatterND cause accepted, other
   failures and non-CUDA targets rejected)
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any
from unittest.mock import MagicMock, patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.tiny_ai_cross_device_parity_gate import (
    CUDA_TF32_PROVIDER_OPTIONS,
    DEFAULT_FP16_TOLERANCE,
    DEFAULT_FP32_TOLERANCE,
    TRANSNET_V2_SCATTERND_GAP,
    CaseResult,
    ParityGateReport,
    create_session,
    evaluate_case,
    evaluate_provider_gap,
    load_fp16_inputs,
    load_fp32_inputs,
    main,
    render_json,
    render_markdown,
    run_parity_gate,
    session_provider_options,
)

EXPECTED_CASES = 3
REPO_ROOT = Path(__file__).resolve().parents[2]
FP32_MODEL = REPO_ROOT / "model" / "tiny" / "vmaf_tiny_v2.onnx"
FP16_MODEL = REPO_ROOT / "model" / "tiny" / "smoke_fp16_v0.onnx"
TRANSNET_MODEL = REPO_ROOT / "model" / "tiny" / "transnet_v2.onnx"
FEATURES_PARQUET = REPO_ROOT / "ai" / "testdata" / "bisect" / "features.parquet"
# The real ORT 1.31.1 CUDA failure: the scatter_nd_impl.cu:323 status wrapped
# in the node context ORT prints, kept independent of the gate's needle constant.
SCATTERND_ERROR = (
    "[ONNXRuntimeError] : 1 : FAIL : Non-zero status code returned while running "
    "ScatterND node. Name:'fork_segmentsum_scatter' Status Message: element_type 6 "
    "not implemented for ScatterND operator."
)


def mock_session(
    providers: list[str],
    input_meta: dict[str, Any] | None = None,
) -> MagicMock:
    """A mocked ORT session reporting the given bound providers.

    The metadata contract the gate reads is get_providers() and get_inputs();
    input_meta overrides the mocked input's kwargs (name, shape, type) when a
    test needs a specific input signature. A session built without input_meta
    leaves get_inputs at its MagicMock default, for the paths that never reach
    it.
    """
    session = MagicMock()
    session.get_providers.return_value = list(providers)
    if input_meta is not None:
        # configure_mock, not the constructor: MagicMock reserves `name` for
        # the mock's display name, so passing it there would leave the mocked
        # input's .name at the auto-generated string instead of the ORT
        # metadata the provider-gap probe keys its feed on.
        mocked_input = MagicMock()
        mocked_input.configure_mock(**input_meta)
        session.get_inputs.return_value = [mocked_input]
    return session


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
    mock_ref_sess = mock_session(["CPUExecutionProvider"], {"name": "features"})

    # Simulate ORT fallback: requested CUDA, but session bound CPU only
    mock_target_sess = mock_session(["CPUExecutionProvider"])

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
    mock_ref_sess = mock_session(["CPUExecutionProvider"], {"name": "features"})
    mock_target_sess = mock_session(["TargetProvider"], {"name": "features"})

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
    mock_ref_sess = mock_session(["CPUExecutionProvider"], {"name": "x"})
    mock_target_sess = mock_session(["TargetProvider"], {"name": "x"})

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
    mock_ref_sess = mock_session(["CPUExecutionProvider"], {"name": "features"})
    mock_target_sess = mock_session(["TargetProvider"], {"name": "features"})

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


def test_session_provider_options_pin_strict_fp32_on_cuda_only() -> None:
    """The CUDA EP carries use_tf32=0; every other provider carries no options."""
    assert CUDA_TF32_PROVIDER_OPTIONS == {"use_tf32": 0}
    assert session_provider_options("CUDAExecutionProvider") == {"use_tf32": 0}
    assert session_provider_options("CPUExecutionProvider") == {}
    assert session_provider_options("OpenVINOExecutionProvider") == {}


def test_create_session_wires_the_tf32_pin_into_every_provider_list() -> None:
    """The session builder passes the pin aligned with the CUDA provider entry."""
    with patch(
        "scripts.ci.tiny_ai_cross_device_parity_gate.ort.InferenceSession",
        return_value=MagicMock(),
    ) as factory:
        create_session(FP32_MODEL, "CUDAExecutionProvider")
        cuda_kwargs = factory.call_args.kwargs
        create_session(FP32_MODEL, "CPUExecutionProvider")
        cpu_kwargs = factory.call_args.kwargs
    assert cuda_kwargs["providers"] == ["CUDAExecutionProvider"]
    assert cuda_kwargs["provider_options"] == [{"use_tf32": 0}]
    assert cpu_kwargs["providers"] == ["CPUExecutionProvider"]
    assert cpu_kwargs["provider_options"] == [{}]


def test_transnet_v2_named_scatternd_failure_is_the_expected_gap() -> None:
    """The measured ScatterND int32 failure passes as PROVIDER_GAP on CUDA."""
    assert TRANSNET_V2_SCATTERND_GAP in SCATTERND_ERROR
    with patch(
        "scripts.ci.tiny_ai_cross_device_parity_gate.create_session",
        side_effect=RuntimeError(SCATTERND_ERROR),
    ):
        case = evaluate_provider_gap(FP32_MODEL, "CUDAExecutionProvider")
    assert case.ok
    assert case.status == "PROVIDER_GAP"
    assert "ScatterND" in case.note


def test_transnet_v2_other_failure_fails_closed() -> None:
    """Any other CUDA failure of the probe still fails the gate."""
    with patch(
        "scripts.ci.tiny_ai_cross_device_parity_gate.create_session",
        side_effect=RuntimeError("CUDA failure 2: out of memory"),
    ):
        case = evaluate_provider_gap(FP32_MODEL, "CUDAExecutionProvider")
    assert not case.ok
    assert case.status == "ERROR"


def test_transnet_v2_named_cause_is_not_accepted_off_cuda() -> None:
    """The named-cause acceptance is bound to the CUDA EP; the CPU leg is unaffected."""
    with patch(
        "scripts.ci.tiny_ai_cross_device_parity_gate.create_session",
        side_effect=RuntimeError(SCATTERND_ERROR),
    ):
        case = evaluate_provider_gap(FP32_MODEL, "CPUExecutionProvider")
    assert not case.ok
    assert case.status == "ERROR"


def test_transnet_v2_successful_execute_reports_the_gap_closed() -> None:
    """A gap-listed model that executes on the target is fine: reported, never a failure."""
    bound = mock_session(
        ["CUDAExecutionProvider"],
        {"name": "frames", "shape": [1, 100, 3, 27, 48], "type": "tensor(float)"},
    )
    bound.run.return_value = [np.zeros((1, 100), dtype=np.float32)]
    with patch("scripts.ci.tiny_ai_cross_device_parity_gate.create_session", return_value=bound):
        case = evaluate_provider_gap(FP32_MODEL, "CUDAExecutionProvider")
    assert case.ok
    assert case.status == "OK"
    assert "gap closed" in case.note


def test_transnet_v2_run_time_scatternd_failure_is_the_expected_gap() -> None:
    """The gap is a Run-time kernel failure: a bound session that fails Run matches."""
    bound = mock_session(
        ["CUDAExecutionProvider"],
        {"name": "frames", "shape": [1, 100, 3, 27, 48], "type": "tensor(float)"},
    )
    bound.run.side_effect = RuntimeError(SCATTERND_ERROR)
    with patch("scripts.ci.tiny_ai_cross_device_parity_gate.create_session", return_value=bound):
        case = evaluate_provider_gap(FP32_MODEL, "CUDAExecutionProvider")
    assert case.ok
    assert case.status == "PROVIDER_GAP"
    assert "ScatterND" in case.note


def test_transnet_v2_run_time_other_failure_fails_closed() -> None:
    """A bound session whose Run fails for any other reason still fails the gate."""
    bound = mock_session(
        ["CUDAExecutionProvider"],
        {"name": "frames", "shape": [1, 100, 3, 27, 48], "type": "tensor(float)"},
    )
    bound.run.side_effect = RuntimeError("Non-zero status code returned while running Conv node")
    with patch("scripts.ci.tiny_ai_cross_device_parity_gate.create_session", return_value=bound):
        case = evaluate_provider_gap(FP32_MODEL, "CUDAExecutionProvider")
    assert not case.ok
    assert case.status == "ERROR"


def test_transnet_v2_unfeedable_input_type_fails_closed() -> None:
    """A probe input the execute probe cannot feed is an error, not a gap."""
    bound = mock_session(
        ["CUDAExecutionProvider"],
        {"name": "x", "shape": [1, 4], "type": "tensor(int64)"},
    )
    with patch("scripts.ci.tiny_ai_cross_device_parity_gate.create_session", return_value=bound):
        case = evaluate_provider_gap(FP32_MODEL, "CUDAExecutionProvider")
    assert not case.ok
    assert case.status == "ERROR"


def test_transnet_v2_silent_fallback_fails_closed() -> None:
    """The probe session falling back to the CPU fails closed, as elsewhere."""
    fallback = mock_session(["CPUExecutionProvider"])
    with patch("scripts.ci.tiny_ai_cross_device_parity_gate.create_session", return_value=fallback):
        case = evaluate_provider_gap(FP32_MODEL, "CUDAExecutionProvider")
    assert not case.ok
    assert case.status == "PROVIDER_UNBOUND"


def test_transnet_v2_missing_model_fails(tmp_path: Path) -> None:
    """A missing transnet_v2 file is an error, not an expected gap."""
    case = evaluate_provider_gap(tmp_path / "absent.onnx", "CUDAExecutionProvider")
    assert not case.ok
    assert case.status == "ERROR"


def test_markdown_surfaces_tf32_pin_and_expected_gap() -> None:
    """The printed summary names the TF32 mode and the per-model provider gaps."""
    report = ParityGateReport(
        reference_provider="CPUExecutionProvider",
        target_provider="CUDAExecutionProvider",
        target_available=True,
        ok=True,
        status="PASS",
        cases=[
            CaseResult(
                name="transnet_v2 provider gap",
                model_path=str(TRANSNET_MODEL),
                precision="execute",
                tolerance=0.0,
                max_abs_error=0.0,
                mean_abs_error=0.0,
                ok=True,
                status="PROVIDER_GAP",
                note="expected gap",
            )
        ],
        tf32_pinned=True,
    )
    md_text = render_markdown(report)
    assert "use_tf32=0 pinned on every CUDAExecutionProvider session" in md_text
    assert "Expected provider gaps**: transnet_v2 provider gap" in md_text
    payload = render_json(report)
    assert payload["tf32_pinned"] is True


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
        transnet_model=TRANSNET_MODEL,
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
    # The transnet_v2 leg is a bind probe: on the CPU EP it simply binds, and the
    # named ScatterND gap logic never applies to a non-CUDA target.
    probe = report.cases[-1]
    assert probe.name == "transnet_v2 provider gap"
    assert "CPUExecutionProvider" in probe.note


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
        transnet_model=TRANSNET_MODEL,
        ref_provider="CPUExecutionProvider",
        target_provider="CPUExecutionProvider",
    )
    json_dict = render_json(report)
    assert json_dict["schema_version"] == 1
    assert json_dict["ok"] is True
    assert json_dict["status"] == "PASS"
    assert json_dict["tf32_pinned"] is False
    assert len(json_dict["cases"]) == EXPECTED_CASES

    md_text = render_markdown(report)
    assert (
        "# Tiny AI Cross-Device Parity Gate Report (T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25)"
        in md_text
    )
    assert "FP32 vmaf_tiny_v2" in md_text
    assert "FP16 smoke_fp16_v0" in md_text
    assert "not pinned (no CUDA session in this run)" in md_text
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
