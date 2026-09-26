# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Bounded, explicit-backend validation for RC1 tester reports."""

from __future__ import annotations

import hashlib
import json
import math
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from vmaf_rc1_tester.probe import REPO_ROOT, DiagnosticReport, SubprocessRunner, _run_cmd

VMAF_EXIT_BACKEND_INIT_FAILED = 100
SMOKE_WIDTH = 576
SMOKE_HEIGHT = 324
SMOKE_FRAME_COUNT = 4
MAX_JSON_BYTES = 1_048_576
MAX_PINNED_JSON_BYTES = 1_048_576
VMAF_SCORE_MIN = 0.0
VMAF_SCORE_MAX = 100.0
OUTPUT_AGGREGATE_TOLERANCE = 1e-6
# ADR-0214 owns 5e-5 for its listed feature metrics. ADR-1342 deliberately
# adopts that same conservative bound for the reporter's overall VMAF score;
# ADR-0214 did not previously define an overall-model VMAF gate.
PARITY_TOLERANCE = 5e-5
MODEL_PARITY_METRICS = (
    "integer_adm2",
    "integer_motion2",
    "integer_vif_scale0",
    "integer_vif_scale1",
    "integer_vif_scale2",
    "integer_vif_scale3",
    "vmaf",
)
DEVICE_SELECTOR_OPTIONS: dict[str, str] = {
    "sycl": "--sycl_device",
    "hip": "--hip_device",
    "metal": "--metal_device",
}


@dataclass(frozen=True)
class ValidationResult:
    """One explicit backend attempt and the evidence needed to replay it."""

    backend_requested: str
    backend_observed: str | None
    requested_by_user: bool
    device_index: int | None
    environment_overrides: dict[str, str]
    verdict: str
    process_exit_code: int | None
    duration_seconds: float
    command_argv: list[str]
    stdout: str
    stderr: str
    json_output: dict[str, Any] | None
    binary_sha256: str
    model_sha256: str
    fixture_sha256: dict[str, str]
    reference_backend: str | None
    parity_tolerance: float | None
    max_abs_score_delta: float | None
    notes: str

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(65536):
            digest.update(chunk)
    return digest.hexdigest()


def _find_smoke_inputs() -> tuple[Path, Path, Path, Path] | None:
    """Return the model, fixture pair, and pinned CPU score snapshot."""
    reference = REPO_ROOT / "testdata" / "ref_576x324_48f.yuv"
    distorted = REPO_ROOT / "testdata" / "dis_576x324_48f.yuv"
    model = REPO_ROOT / "model" / "vmaf_v0.6.1.json"
    cpu_snapshot = REPO_ROOT / "testdata" / "scores_cpu_576.json"
    if all(path.is_file() for path in (reference, distorted, model, cpu_snapshot)):
        return reference, distorted, model, cpu_snapshot
    return None


def _skipped_result(
    backend: str,
    binary_sha256: str,
    notes: str,
    device_index: int,
    requested_by_user: bool,
) -> ValidationResult:
    return ValidationResult(
        backend_requested=backend,
        backend_observed=None,
        requested_by_user=requested_by_user,
        device_index=None if backend == "cpu" else device_index,
        environment_overrides=_environment_overrides(backend, device_index),
        verdict="SKIPPED",
        process_exit_code=None,
        duration_seconds=0.0,
        command_argv=[],
        stdout="",
        stderr="",
        json_output=None,
        binary_sha256=binary_sha256,
        model_sha256="",
        fixture_sha256={},
        reference_backend=None,
        parity_tolerance=None,
        max_abs_score_delta=None,
        notes=notes,
    )


def _read_json(
    path: Path,
    *,
    max_bytes: int | None = None,
) -> tuple[dict[str, Any] | None, str | None]:
    byte_limit = MAX_JSON_BYTES if max_bytes is None else max_bytes
    if not path.is_file():
        return None, "vmaf returned success without creating JSON output"
    try:
        with path.open("rb") as stream:
            payload = stream.read(byte_limit + 1)
    except OSError as exc:
        return None, f"vmaf produced unreadable JSON output: {exc}"
    if len(payload) > byte_limit:
        return None, f"vmaf JSON output exceeded {byte_limit} bytes"
    try:
        data = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, ValueError, RecursionError) as exc:
        return None, f"vmaf produced unreadable JSON output: {exc}"
    if not isinstance(data, dict):
        return None, "vmaf JSON output was not an object"
    return data, None


def _is_finite_number(value: object) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def _is_valid_vmaf_score(value: object) -> bool:
    return _is_finite_number(value) and VMAF_SCORE_MIN <= value <= VMAF_SCORE_MAX


def _metric_vectors(data: dict[str, Any]) -> dict[str, tuple[float, ...]]:
    frames = data["frames"]
    return {
        metric: tuple(float(frame["metrics"][metric]) for frame in frames)
        for metric in MODEL_PARITY_METRICS
    }


def _expected_pool(scores: tuple[float, ...]) -> dict[str, float]:
    inverse_sum = sum(1.0 / (score + 1.0) for score in scores)
    return {
        "min": min(scores),
        "max": max(scores),
        "mean": sum(scores) / len(scores),
        "harmonic_mean": len(scores) / inverse_sum - 1.0 if inverse_sum > 0.0 else 0.0,
    }


def _frame_evidence_error(frames: object) -> str | None:
    if not isinstance(frames, list) or len(frames) != SMOKE_FRAME_COUNT:
        return f"vmaf JSON must contain exactly {SMOKE_FRAME_COUNT} scored frames"
    for expected_index, frame in enumerate(frames):
        if not isinstance(frame, dict) or frame.get("frameNum") != expected_index:
            return "vmaf JSON frame numbers must be the consecutive range 0..3"
        metrics = frame.get("metrics")
        if not isinstance(metrics, dict):
            return "vmaf JSON frame metrics are missing or malformed"
        for metric in MODEL_PARITY_METRICS:
            value = metrics.get(metric)
            if not _is_finite_number(value):
                return f"vmaf JSON frame metric {metric!r} is missing or non-finite"
        if not _is_valid_vmaf_score(metrics.get("vmaf")):
            return "vmaf JSON frame metric is outside the model range [0, 100]"
    return None


def _pooled_evidence_error(data: dict[str, Any]) -> str | None:
    pooled = data.get("pooled_metrics")
    pooled_vmaf = pooled.get("vmaf") if isinstance(pooled, dict) else None
    required = ("min", "max", "mean", "harmonic_mean")
    if not isinstance(pooled_vmaf, dict) or any(
        not _is_valid_vmaf_score(pooled_vmaf.get(key)) for key in required
    ):
        return (
            "vmaf JSON pooled metrics are missing, malformed, non-finite, "
            "or outside the model range [0, 100]"
        )
    assert isinstance(pooled_vmaf, dict)
    scores = _metric_vectors(data)["vmaf"]
    expected_pool = _expected_pool(scores)
    for key in required:
        if not math.isclose(
            float(pooled_vmaf[key]),
            expected_pool[key],
            rel_tol=0.0,
            abs_tol=OUTPUT_AGGREGATE_TOLERANCE,
        ):
            return "vmaf JSON pooled metrics are inconsistent with the four frame scores"
    return None


def _score_evidence_error(data: dict[str, Any]) -> str | None:
    return _frame_evidence_error(data.get("frames")) or _pooled_evidence_error(data)


def _parity_result(
    actual: dict[str, Any],
    reference: dict[str, Any],
) -> tuple[str | None, float]:
    actual_vectors = _metric_vectors(actual)
    reference_vectors = _metric_vectors(reference)
    max_delta = 0.0
    worst_metric = ""
    worst_frame = 0
    for metric in MODEL_PARITY_METRICS:
        for frame, (actual_score, reference_score) in enumerate(
            zip(actual_vectors[metric], reference_vectors[metric], strict=True)
        ):
            delta = abs(actual_score - reference_score)
            if delta > max_delta:
                max_delta = delta
                worst_metric = metric
                worst_frame = frame
    if max_delta > PARITY_TOLERANCE:
        return (
            (
                f"score parity exceeded {PARITY_TOLERANCE:g}: {worst_metric} frame "
                f"{worst_frame} differed by {max_delta:.9g}"
            ),
            max_delta,
        )
    return None, max_delta


def _load_pinned_smoke_data(path: Path) -> tuple[dict[str, Any] | None, str | None]:
    snapshot, error = _read_json(path, max_bytes=MAX_PINNED_JSON_BYTES)
    if error or snapshot is None:
        return None, error or "pinned CPU snapshot is unavailable"
    frames = snapshot.get("frames")
    if not isinstance(frames, list) or len(frames) < SMOKE_FRAME_COUNT:
        return None, "pinned CPU snapshot does not contain four frames"
    selected_frames = frames[:SMOKE_FRAME_COUNT]
    try:
        scores = tuple(float(frame["metrics"]["vmaf"]) for frame in selected_frames)
    except (KeyError, OverflowError, TypeError, ValueError):
        return None, "pinned CPU snapshot has malformed frame metrics"
    if any(not _is_valid_vmaf_score(score) for score in scores):
        return None, "pinned CPU snapshot has an invalid VMAF score"
    pinned = {
        "frames": selected_frames,
        "pooled_metrics": {"vmaf": _expected_pool(scores)},
    }
    if score_error := _score_evidence_error(pinned):
        return None, f"pinned CPU snapshot is invalid: {score_error}"
    return pinned, None


def _environment_overrides(backend: str, device_index: int) -> dict[str, str]:
    if backend == "cuda":
        return {"CUDA_VISIBLE_DEVICES": str(device_index)}
    return {}


def _classify_smoke_outcome(
    *,
    rc: int,
    backend: str,
    observed: str | None,
    json_data: dict[str, Any] | None,
    json_error: str | None,
    reference_data: dict[str, Any] | None,
    reference_backend: str | None,
) -> tuple[str, str, float | None]:
    if rc == VMAF_EXIT_BACKEND_INIT_FAILED:
        return (
            "BACKEND_UNAVAILABLE",
            f"The explicitly requested {backend} backend could not initialize (exit 100).",
            None,
        )
    if rc != 0:
        return "FAIL", f"vmaf exited with status {rc}.", None
    if json_error:
        return "FAIL", json_error, None
    if observed != backend:
        return "FAIL", f"Requested backend {backend!r}, but JSON reported {observed!r}.", None
    if json_data and (score_error := _score_evidence_error(json_data)):
        return "FAIL", score_error, None
    if json_data is None:
        return "FAIL", "vmaf JSON output was unavailable.", None
    if reference_data is None:
        return (
            "FAIL",
            "No passing CPU reference was available for the accelerator parity check.",
            None,
        )
    parity_error, max_delta = _parity_result(json_data, reference_data)
    if parity_error:
        return "FAIL", parity_error, max_delta
    notes = (
        f"The requested {backend} backend state initialized and its first four model-metric "
        f"frames matched {reference_backend} within {PARITY_TOLERANCE:g} absolute tolerance."
    )
    return "PASS", notes, max_delta


def _evaluate_smoke_run(
    *,
    rc: int,
    out: str,
    err: str,
    json_path: Path,
    backend: str,
    device_index: int,
    environment_overrides: dict[str, str],
    duration: float,
    command: list[str],
    binary_sha256: str,
    model_sha256: str,
    fixture_sha256: dict[str, str],
    reference_data: dict[str, Any] | None,
    reference_backend: str | None,
    requested_by_user: bool,
) -> ValidationResult:
    json_data, json_error = _read_json(json_path)
    observed = (
        str(json_data.get("backend_used")) if json_data and json_data.get("backend_used") else None
    )
    verdict, notes, max_delta = _classify_smoke_outcome(
        rc=rc,
        backend=backend,
        observed=observed,
        json_data=json_data,
        json_error=json_error,
        reference_data=reference_data,
        reference_backend=reference_backend,
    )
    return ValidationResult(
        backend_requested=backend,
        backend_observed=observed,
        requested_by_user=requested_by_user,
        device_index=None if backend == "cpu" else device_index,
        environment_overrides=environment_overrides,
        verdict=verdict,
        process_exit_code=rc,
        duration_seconds=round(duration, 3),
        command_argv=command,
        stdout=out,
        stderr=err,
        json_output=json_data,
        binary_sha256=binary_sha256,
        model_sha256=model_sha256,
        fixture_sha256=fixture_sha256,
        reference_backend=reference_backend,
        parity_tolerance=PARITY_TOLERANCE if reference_data is not None else None,
        max_abs_score_delta=max_delta,
        notes=notes,
    )


def _smoke_command(
    binary: str,
    reference: Path,
    distorted: Path,
    model: Path,
    output: Path,
    backend: str,
    device_index: int,
) -> list[str]:
    command = [
        binary,
        "--reference",
        str(reference),
        "--distorted",
        str(distorted),
        "--width",
        str(SMOKE_WIDTH),
        "--height",
        str(SMOKE_HEIGHT),
        "--pixel_format",
        "420",
        "--bitdepth",
        "8",
        "--frame_cnt",
        str(SMOKE_FRAME_COUNT),
        "--model",
        f"path={model}",
    ]
    if selector := DEVICE_SELECTOR_OPTIONS.get(backend):
        command.extend((selector, str(device_index)))
    command.extend(("--backend", backend, "--json", "--output", str(output), "--quiet"))
    return command


def _comparison_reference(
    backend: str,
    pinned_data: dict[str, Any],
    cpu_reference: ValidationResult | None,
) -> tuple[dict[str, Any] | None, str]:
    if backend == "cpu":
        return pinned_data, "pinned testdata/scores_cpu_576.json"
    if cpu_reference and cpu_reference.verdict == "PASS":
        return cpu_reference.json_output, "cpu"
    return None, "cpu"


def _fixture_hashes(reference: Path, distorted: Path, cpu_snapshot: Path) -> dict[str, str]:
    return {
        "reference": _sha256(reference),
        "distorted": _sha256(distorted),
        "pinned_cpu_scores": _sha256(cpu_snapshot),
    }


def _run_smoke_process(
    command: list[str],
    environment_overrides: dict[str, str],
    runner: SubprocessRunner | None,
    timeout: float,
) -> tuple[int, str, str, float]:
    started = time.monotonic()
    rc, out, err = _run_cmd(
        command,
        runner=runner,
        timeout=timeout,
        environment_overrides=environment_overrides,
    )
    return rc, out, err, time.monotonic() - started


def _execute_smoke_validation(
    report: DiagnosticReport,
    backend: str,
    inputs: tuple[Path, Path, Path, Path],
    runner: SubprocessRunner | None = None,
    timeout: float = 30.0,
    device_index: int = 0,
    cpu_reference: ValidationResult | None = None,
    requested_by_user: bool = True,
) -> ValidationResult:
    reference, distorted, model, cpu_snapshot = inputs
    binary = report.vmaf_binary
    fixture_hashes = _fixture_hashes(reference, distorted, cpu_snapshot)
    pinned_data, pinned_error = _load_pinned_smoke_data(cpu_snapshot)
    if pinned_error:
        return _skipped_result(
            backend,
            binary.sha256,
            pinned_error,
            device_index,
            requested_by_user,
        )
    assert pinned_data is not None
    reference_data, reference_backend = _comparison_reference(backend, pinned_data, cpu_reference)
    with tempfile.TemporaryDirectory(prefix="vmafx_rc1_") as temporary:
        json_path = Path(temporary) / "smoke.json"
        command = _smoke_command(
            binary.path,
            reference,
            distorted,
            model,
            json_path,
            backend,
            device_index,
        )
        environment_overrides = _environment_overrides(backend, device_index)
        rc, out, err, duration = _run_smoke_process(command, environment_overrides, runner, timeout)
        return _evaluate_smoke_run(
            rc=rc,
            out=out,
            err=err,
            json_path=json_path,
            backend=backend,
            device_index=device_index,
            environment_overrides=environment_overrides,
            duration=duration,
            command=command,
            binary_sha256=binary.sha256,
            model_sha256=_sha256(model),
            fixture_sha256=fixture_hashes,
            reference_data=reference_data,
            reference_backend=reference_backend,
            requested_by_user=requested_by_user,
        )


def run_smoke_validation(
    report: DiagnosticReport,
    backend: str,
    runner: SubprocessRunner | None = None,
    timeout: float = 30.0,
    device_index: int = 0,
    cpu_reference: ValidationResult | None = None,
    requested_by_user: bool = True,
) -> ValidationResult:
    """Run four frames and fail closed unless score correctness is demonstrated."""
    if device_index < 0:
        raise ValueError("device_index must be non-negative")
    binary = report.vmaf_binary
    if not binary.exists or not binary.path:
        return _skipped_result(
            backend,
            binary.sha256,
            "vmaf executable not found or not executable.",
            device_index,
            requested_by_user,
        )
    inputs = _find_smoke_inputs()
    if inputs is None:
        return _skipped_result(
            backend,
            binary.sha256,
            "Checked-in model, fixture pair, or CPU score snapshot is missing.",
            device_index,
            requested_by_user,
        )
    return _execute_smoke_validation(
        report,
        backend,
        inputs,
        runner,
        timeout,
        device_index,
        cpu_reference,
        requested_by_user,
    )
