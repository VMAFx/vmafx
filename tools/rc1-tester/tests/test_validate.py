# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for fail-closed explicit-backend validation."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from unittest import mock

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester.probe import (
    REPO_ROOT,
    CPUInfo,
    DiagnosticReport,
    HardwareProbes,
    OSInfo,
    ToolchainInfo,
    VmafBinaryInfo,
)
from vmaf_rc1_tester.validate import (
    MODEL_PARITY_METRICS,
    PARITY_TOLERANCE,
    SMOKE_FRAME_COUNT,
    VMAF_EXIT_BACKEND_INIT_FAILED,
    VMAF_SCORE_MAX,
    VMAF_SCORE_MIN,
    ValidationResult,
    run_smoke_validation,
)

GENERIC_FAILURE_EXIT = 7


class FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = "") -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _report(binary: Path | None) -> DiagnosticReport:
    binary_info = VmafBinaryInfo(
        path=str(binary) if binary else "",
        exists=binary is not None,
        version="3.2.0",
        sha256="a" * 64 if binary else "",
        accepted_backend_selectors=["cpu", "cuda", "sycl", "hip", "metal"],
    )
    return DiagnosticReport(
        OSInfo("Linux", "6.8", "x86_64", "Test Linux", False),
        CPUInfo("Test CPU", 8, True, False, False),
        ToolchainInfo(*(["not installed"] * 8)),
        HardwareProbes(),
        binary_info,
    )


def _fixtures(tmp_path: Path) -> tuple[Path, Path, Path, Path]:
    reference = tmp_path / "reference.yuv"
    distorted = tmp_path / "distorted.yuv"
    model = tmp_path / "model.json"
    cpu_snapshot = tmp_path / "scores_cpu_576.json"
    reference.write_bytes(b"reference")
    distorted.write_bytes(b"distorted")
    model.write_text("{}", encoding="utf-8")
    cpu_snapshot.write_text(json.dumps(_score_json("cpu")), encoding="utf-8")
    return reference, distorted, model, cpu_snapshot


def _binary(tmp_path: Path) -> Path:
    binary = tmp_path / "vmaf"
    binary.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    binary.chmod(0o755)
    return binary


def _score_json(backend: str, score: float = 92.4) -> dict[str, object]:
    scores = [score] * SMOKE_FRAME_COUNT
    return {
        "frames": [
            {
                "frameNum": frame,
                "metrics": {metric: value for metric in MODEL_PARITY_METRICS},
            }
            for frame, value in enumerate(scores)
        ],
        "pooled_metrics": {
            "vmaf": {
                "min": min(scores),
                "max": max(scores),
                "mean": score,
                "harmonic_mean": score,
            }
        },
        "backend_used": backend,
    }


def _cpu_reference(payload: dict[str, object] | None = None) -> ValidationResult:
    return ValidationResult(
        backend_requested="cpu",
        backend_observed="cpu",
        requested_by_user=False,
        device_index=None,
        environment_overrides={},
        verdict="PASS",
        process_exit_code=0,
        duration_seconds=0.1,
        command_argv=["vmaf", "--backend", "cpu"],
        stdout="",
        stderr="",
        json_output=payload or _score_json("cpu"),
        binary_sha256="a" * 64,
        model_sha256="b" * 64,
        fixture_sha256={},
        reference_backend="pinned testdata/scores_cpu_576.json",
        parity_tolerance=PARITY_TOLERANCE,
        max_abs_score_delta=0.0,
        notes="test CPU reference",
    )


def test_score_bounds_match_pinned_model() -> None:
    model = json.loads((REPO_ROOT / "model" / "vmaf_v0.6.1.json").read_text(encoding="utf-8"))
    expected = [VMAF_SCORE_MIN, VMAF_SCORE_MAX]
    assert model["param_dict"]["score_clip"] == expected
    assert model["model_dict"]["score_clip"] == expected


def test_pinned_cpu_smoke_window_reaches_temporal_frame_three() -> None:
    snapshot = json.loads(
        (REPO_ROOT / "testdata" / "scores_cpu_576.json").read_text(encoding="utf-8")
    )
    frames = snapshot["frames"][:SMOKE_FRAME_COUNT]
    assert [frame["metrics"]["vmaf"] for frame in frames] == [
        97.428043,
        97.428043,
        97.428043,
        100.0,
    ]
    assert frames[3]["metrics"]["integer_motion2"] == 20.447048


def test_missing_binary_is_incomplete_not_pass() -> None:
    result = run_smoke_validation(_report(None), backend="cuda")
    assert result.verdict == "SKIPPED"
    assert result.process_exit_code is None


def test_missing_fixture_or_model_is_incomplete_not_version_pass(tmp_path: Path) -> None:
    with mock.patch("vmaf_rc1_tester.validate._find_smoke_inputs", return_value=None):
        result = run_smoke_validation(_report(_binary(tmp_path)), backend="cpu")
    assert result.verdict == "SKIPPED"
    assert result.command_argv == []
    assert "model, fixture pair, or CPU score snapshot" in result.notes


def test_success_requires_matching_backend_and_real_score_schema(tmp_path: Path) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(_score_json("cuda")))
        return FakeCompleted(0, "four frames processed", "")

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(
            _report(binary),
            backend="cuda",
            runner=fake_runner,
            device_index=2,
            cpu_reference=_cpu_reference(),
        )
    assert result.verdict == "PASS"
    assert result.backend_observed == "cuda"
    assert result.process_exit_code == 0
    assert result.command_argv[result.command_argv.index("--backend") + 1] == "cuda"
    assert result.command_argv[result.command_argv.index("--frame_cnt") + 1] == "4"
    assert result.device_index == 2
    assert result.environment_overrides == {"CUDA_VISIBLE_DEVICES": "2"}
    assert result.command_argv[result.command_argv.index("--model") + 1].startswith("path=")
    assert result.model_sha256
    assert set(result.fixture_sha256) == {"reference", "distorted", "pinned_cpu_scores"}


def test_success_without_json_fails_closed(tmp_path: Path) -> None:
    binary = _binary(tmp_path)
    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(
            _report(binary), backend="cpu", runner=lambda *_args, **_kwargs: FakeCompleted(0)
        )
    assert result.verdict == "FAIL"
    assert result.process_exit_code == 0
    assert "without creating JSON" in result.notes


def test_backend_mismatch_fails_closed(tmp_path: Path) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(_score_json("cpu")))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(_report(binary), backend="hip", runner=fake_runner)
    assert result.verdict == "FAIL"
    assert result.backend_observed == "cpu"
    assert "Requested backend 'hip'" in result.notes


def test_exit_100_remains_backend_unavailable(tmp_path: Path) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps({"backend_requested": "hip", "exit_code": 100}))
        return FakeCompleted(VMAF_EXIT_BACKEND_INIT_FAILED, "", "HIP init failed")

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(_report(binary), backend="hip", runner=fake_runner)
    assert result.verdict == "BACKEND_UNAVAILABLE"
    assert result.process_exit_code == VMAF_EXIT_BACKEND_INIT_FAILED


def test_generic_failure_keeps_process_evidence(tmp_path: Path) -> None:
    binary = _binary(tmp_path)
    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(
            _report(binary),
            backend="cpu",
            runner=lambda *_args, **_kwargs: FakeCompleted(GENERIC_FAILURE_EXIT, "", "bad input"),
        )
    assert result.verdict == "FAIL"
    assert result.process_exit_code == GENERIC_FAILURE_EXIT
    assert result.stderr == "bad input"


@pytest.mark.parametrize(
    ("payload", "message"),
    [
        ({"backend_used": "cuda"}, "exactly 4 scored frames"),
        (
            {
                "backend_used": "cuda",
                "frames": _score_json("cuda")["frames"],
                "pooled_metrics": {"vmaf": 92.4},
            },
            "pooled metrics",
        ),
        (_score_json("cuda", float("nan")), "non-finite"),
        (_score_json("cuda", 10**1000), "non-finite"),
        (_score_json("cuda", 1e300), "outside the model range"),
        (_score_json("cuda", -0.1), "outside the model range"),
        (_score_json("cuda", 100.1), "outside the model range"),
    ],
)
def test_incomplete_or_nonfinite_score_evidence_fails_closed(
    payload: dict[str, object], message: str, tmp_path: Path
) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(payload))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(_report(binary), backend="cuda", runner=fake_runner)
    assert result.verdict == "FAIL"
    assert message in result.notes


def test_inconsistent_four_frame_pool_fails_closed(tmp_path: Path) -> None:
    binary = _binary(tmp_path)
    payload = _score_json("cpu", 92.4)
    pooled = payload["pooled_metrics"]
    assert isinstance(pooled, dict)
    pooled_vmaf = pooled["vmaf"]
    assert isinstance(pooled_vmaf, dict)
    pooled_vmaf["mean"] = 10.0

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(payload))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(_report(binary), backend="cpu", runner=fake_runner)
    assert result.verdict == "FAIL"
    assert "inconsistent with the four frame scores" in result.notes


def test_materially_wrong_but_in_range_cpu_scores_fail_pinned_reference(tmp_path: Path) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(_score_json("cpu", 10.0)))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(_report(binary), backend="cpu", runner=fake_runner)
    assert result.verdict == "FAIL"
    assert result.max_abs_score_delta == pytest.approx(82.4)
    assert "score parity exceeded" in result.notes


def test_materially_wrong_but_in_range_accelerator_scores_fail_cpu_parity(
    tmp_path: Path,
) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(_score_json("cuda", 92.401)))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(
            _report(binary),
            backend="cuda",
            runner=fake_runner,
            cpu_reference=_cpu_reference(),
        )
    assert result.verdict == "FAIL"
    assert result.max_abs_score_delta == pytest.approx(0.001)
    assert "score parity exceeded" in result.notes


def test_accelerator_score_inside_existing_parity_contract_passes(tmp_path: Path) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(_score_json("cuda", 92.40004)))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(
            _report(binary),
            backend="cuda",
            runner=fake_runner,
            cpu_reference=_cpu_reference(),
        )
    assert result.verdict == "PASS"
    assert result.max_abs_score_delta == pytest.approx(4e-5)
    assert result.parity_tolerance == PARITY_TOLERANCE


def test_accelerator_without_passing_cpu_reference_fails_closed(tmp_path: Path) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(_score_json("hip")))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(_report(binary), backend="hip", runner=fake_runner)
    assert result.verdict == "FAIL"
    assert "No passing CPU reference" in result.notes


def test_missing_temporal_metric_fails_closed(tmp_path: Path) -> None:
    binary = _binary(tmp_path)
    payload = _score_json("cpu")
    frames = payload["frames"]
    assert isinstance(frames, list)
    del frames[3]["metrics"]["integer_motion2"]

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(payload))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(_report(binary), backend="cpu", runner=fake_runner)
    assert result.verdict == "FAIL"
    assert "integer_motion2" in result.notes


@pytest.mark.parametrize(
    ("backend", "selector"),
    [("sycl", "--sycl_device"), ("hip", "--hip_device"), ("metal", "--metal_device")],
)
def test_accelerator_device_index_is_passed_explicitly(
    backend: str,
    selector: str,
    tmp_path: Path,
) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        assert command[command.index(selector) + 1] == "3"
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(_score_json(backend)))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(
            _report(binary),
            backend=backend,
            runner=fake_runner,
            device_index=3,
            cpu_reference=_cpu_reference(),
        )
    assert result.verdict == "PASS"
    assert result.device_index == 3
    assert result.environment_overrides == {}


def test_cpu_ignores_accelerator_device_index(tmp_path: Path) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        assert not any(argument.endswith("_device") for argument in command)
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(_score_json("cpu")))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(
            _report(binary), backend="cpu", runner=fake_runner, device_index=4
        )
    assert result.verdict == "PASS"
    assert result.device_index is None
    assert result.environment_overrides == {}


def test_cuda_device_index_is_passed_through_bounded_environment(tmp_path: Path) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **kwargs: object) -> FakeCompleted:
        environment = kwargs["env"]
        assert isinstance(environment, dict)
        assert environment["CUDA_VISIBLE_DEVICES"] == "5"
        output = Path(command[command.index("--output") + 1])
        output.write_text(json.dumps(_score_json("cuda")))
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(
            _report(binary),
            backend="cuda",
            runner=fake_runner,
            device_index=5,
            cpu_reference=_cpu_reference(),
        )
    assert result.verdict == "PASS"
    assert result.environment_overrides == {"CUDA_VISIBLE_DEVICES": "5"}


def test_negative_device_index_is_rejected() -> None:
    with pytest.raises(ValueError, match="non-negative"):
        run_smoke_validation(_report(None), backend="cuda", device_index=-1)


def test_integer_limit_json_failure_is_reported_not_raised(tmp_path: Path) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text('{"backend_used":"cuda","frames":[],"poison":' + ("9" * 5000) + "}")
        return FakeCompleted(0)

    with mock.patch(
        "vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)
    ):
        result = run_smoke_validation(_report(binary), backend="cuda", runner=fake_runner)
    assert result.verdict == "FAIL"
    assert "unreadable JSON output" in result.notes


def test_oversized_json_output_fails_closed(tmp_path: Path) -> None:
    binary = _binary(tmp_path)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        output = Path(command[command.index("--output") + 1])
        output.write_text("{" + (" " * 64) + "}")
        return FakeCompleted(0)

    with (
        mock.patch("vmaf_rc1_tester.validate._find_smoke_inputs", return_value=_fixtures(tmp_path)),
        mock.patch("vmaf_rc1_tester.validate.MAX_JSON_BYTES", 32),
    ):
        result = run_smoke_validation(_report(binary), backend="cuda", runner=fake_runner)
    assert result.verdict == "FAIL"
    assert "exceeded 32 bytes" in result.notes
