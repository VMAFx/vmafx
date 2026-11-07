#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tiny AI cross-device numerical parity gate (T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25).

Compares ONNX model inference outputs across execution providers (default:
CPUExecutionProvider vs CUDAExecutionProvider) to enforce documented numerical
tolerances:
- FP32: absolute error <= 1e-4 on model/tiny/vmaf_tiny_v2.onnx
- FP16: absolute error <= 1e-2 on model/tiny/smoke_fp16_v0.onnx
- transnet_v2: a zeros-input execute probe against the target provider, not a
  numerical case (its CUDA leg cannot execute on ORT 1.31.1, see
  TRANSNET_V2_SCATTERND_GAP)

Every CUDAExecutionProvider session is created with the provider option
use_tf32=0: ORT 1.31.1 leaves TF32 enabled by default in the CUDA EP and TF32
GEMM/conv breaks the documented 1e-4 fp32 bound, while the session-config entry
ep.cuda.use_tf32 is silently ignored by that release (issue #2947, comment
6696661349).

Fail-closed contract:
- If a requested target provider is absent from onnxruntime.get_available_providers(),
  the gate fails closed (exit code 1) unless --allow-missing-provider is passed.
- If onnxruntime silently falls back to CPU for the target session, the gate
  detects the provider mismatch and fails closed.
- If numerical delta exceeds tolerance, the gate fails closed.
- The transnet_v2 probe fails closed on every failure except the one named CUDA
  ScatterND gap recorded in TRANSNET_V2_SCATTERND_GAP.
- Generates JSON summary (--json-out) and Markdown report (--md-out).
"""

from __future__ import annotations

import argparse
import importlib
import json
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
import onnxruntime as ort

try:
    pd: Any = importlib.import_module("pandas")
except ImportError:
    pd = None

CANONICAL_6_FEATURES = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)
DEFAULT_FP32_TOLERANCE = 1e-4
DEFAULT_FP16_TOLERANCE = 1e-2
DEFAULT_REF_PROVIDER = "CPUExecutionProvider"
DEFAULT_TARGET_PROVIDER = "CUDAExecutionProvider"
# ORT 1.31.1 ships the CUDA EP with TF32 enabled by default; TF32 GEMM/conv
# pushed 13/24 fp32 tiny models past the documented 1e-4 bound (drift
# 6e-04..3.9e-02 at CI-like batch sizes) while use_tf32=0 passed all 24 (worst
# 3.8e-05). The session-config entry ep.cuda.use_tf32 is silently ignored by
# 1.31.1, so the pin must ride the provider option list (issue #2947, comment
# 6696661349).
CUDA_TF32_PROVIDER_OPTIONS: dict[str, int] = {"use_tf32": 0}
# transnet_v2 binds the CUDA EP in ORT 1.31.1 but cannot execute there: the
# ScatterND node fork_segmentsum_scatter fails at Run time with this status
# message (element type 6 is ONNX int32), deterministically on both local
# measurement GPUs; the CPU EP is unaffected. The gate runs the model's
# target-provider leg as a zeros-input execute probe, records that one named
# failure as the model's expected provider gap, and fails on every other
# transnet_v2 failure (issue #2947, comment 6696661349).
TRANSNET_V2_MODEL_ID = "transnet_v2"
TRANSNET_V2_SCATTERND_GAP = "element_type 6 not implemented for ScatterND operator"


@dataclass
class CaseResult:
    name: str
    model_path: str
    precision: str
    tolerance: float
    max_abs_error: float
    mean_abs_error: float
    ok: bool
    status: str
    note: str = ""


@dataclass
class ParityGateReport:
    reference_provider: str
    target_provider: str
    target_available: bool
    ok: bool
    status: str
    cases: list[CaseResult] = field(default_factory=list)
    note: str = ""
    tf32_pinned: bool = False


def load_fp32_inputs(features_path: Path, max_rows: int = 256) -> np.ndarray:
    """Load the canonical-6 feature rows of the fixture parquet.

    A missing fixture is an error, never a substitute: a gate that compared
    providers on invented inputs would pass without measuring the model's real
    input distribution.
    """
    if not features_path.is_file():
        raise FileNotFoundError(f"feature fixture not found: {features_path}")
    if pd is None:
        raise RuntimeError(f"pandas is required to read features from {features_path}")
    df = pd.read_parquet(features_path)
    missing = [c for c in CANONICAL_6_FEATURES if c not in df.columns]
    if missing:
        raise ValueError(f"{features_path} missing canonical columns: {missing}")
    arr = df[list(CANONICAL_6_FEATURES)].to_numpy(dtype=np.float32)[:max_rows]
    if arr.shape[0] == 0:
        raise ValueError(f"{features_path} holds no rows")
    return arr


def load_fp16_inputs() -> np.ndarray:
    """Return deterministic float16 test tensor for rank-4 smoke model [1, 1, 2, 2]."""
    return np.array([[[[0.125, 0.25], [0.5, 0.875]]]], dtype=np.float16)


def session_provider_options(provider: str) -> dict[str, int]:
    """Provider options for one session of a pinned-provider run.

    The CUDA EP pins strict fp32 (see CUDA_TF32_PROVIDER_OPTIONS); every other
    provider runs on its defaults.
    """
    if provider == DEFAULT_TARGET_PROVIDER:
        return dict(CUDA_TF32_PROVIDER_OPTIONS)
    return {}


def create_session(model_path: Path, provider: str) -> ort.InferenceSession:
    """Create an ONNX Runtime inference session pinned to a single provider."""
    opts = ort.SessionOptions()
    opts.inter_op_num_threads = 1
    opts.intra_op_num_threads = 1
    return ort.InferenceSession(
        str(model_path),
        sess_options=opts,
        providers=[provider],
        provider_options=[session_provider_options(provider)],
    )


def run_session_inference(
    sess: ort.InferenceSession, input_name: str, input_data: np.ndarray
) -> np.ndarray:
    """Execute session and return primary output ndarray."""
    out = sess.run(None, {input_name: input_data})[0]
    if not isinstance(out, np.ndarray):
        raise TypeError(f"Expected numpy.ndarray output, got {type(out).__name__}")
    return out


def _failed_case(
    name: str, model_path: Path, precision: str, tolerance: float, status: str, note: str
) -> CaseResult:
    """A case that did not produce a comparison."""
    return CaseResult(
        name=name,
        model_path=str(model_path),
        precision=precision,
        tolerance=tolerance,
        max_abs_error=0.0,
        mean_abs_error=0.0,
        ok=False,
        status=status,
        note=note,
    )


def evaluate_case(
    name: str,
    model_path: Path,
    precision: str,
    tolerance: float,
    input_data: np.ndarray,
    ref_provider: str,
    target_provider: str,
) -> CaseResult:
    """Run model through reference and target providers, computing max absolute error."""

    def fail(status: str, note: str) -> CaseResult:
        return _failed_case(name, model_path, precision, tolerance, status, note)

    if not model_path.is_file():
        return fail("ERROR", f"Model file not found: {model_path}")
    try:
        ref_sess = create_session(model_path, ref_provider)
        target_sess = create_session(model_path, target_provider)
        # Fail-closed check: verify target provider was actually bound
        bound = target_sess.get_providers()
        if target_provider not in bound:
            return fail(
                "PROVIDER_UNBOUND",
                f"Target provider '{target_provider}' failed to bind; bound providers: {bound}",
            )
        in_name = ref_sess.get_inputs()[0].name
        ref_out = run_session_inference(ref_sess, in_name, input_data)
        target_out = run_session_inference(target_sess, in_name, input_data)
    except Exception as exc:  # pragma: no cover
        return fail("ERROR", f"Exception during inference: {exc}")
    diff = np.abs(target_out.astype(np.float64) - ref_out.astype(np.float64))
    max_err = float(np.max(diff)) if diff.size > 0 else 0.0
    mean_err = float(np.mean(diff)) if diff.size > 0 else 0.0
    within_bound = max_err <= tolerance
    return CaseResult(
        name=name,
        model_path=str(model_path),
        precision=precision,
        tolerance=tolerance,
        max_abs_error=max_err,
        mean_abs_error=mean_err,
        ok=within_bound,
        status="OK" if within_bound else "FAIL",
        note="" if within_bound else f"max error {max_err:.3e} exceeds tolerance {tolerance:.3e}",
    )


def evaluate_provider_gap(model_path: Path, target_provider: str) -> CaseResult:
    """transnet_v2's target-provider leg: a zeros-input execute probe.

    The model binds the CUDA EP in ORT 1.31.1 but cannot execute there (see
    TRANSNET_V2_SCATTERND_GAP), so this leg runs one zeros input on the target
    provider and judges only that outcome: the one named CUDA failure is the
    expected gap and passes as PROVIDER_GAP, a successful run is reported as
    the gap closing, and anything else fails the gate. The zeros input is never
    compared numerically; this leg measures executability, not parity.
    """
    name = f"{TRANSNET_V2_MODEL_ID} provider gap"

    def case(status: str, ok: bool, note: str) -> CaseResult:
        return CaseResult(
            name=name,
            model_path=str(model_path),
            precision="execute",
            tolerance=0.0,
            max_abs_error=0.0,
            mean_abs_error=0.0,
            ok=ok,
            status=status,
            note=note,
        )

    if not model_path.is_file():
        return case("ERROR", False, f"Model file not found: {model_path}")
    try:
        sess = create_session(model_path, target_provider)
        bound = sess.get_providers()
        if target_provider not in bound:
            return case(
                "PROVIDER_UNBOUND",
                False,
                f"Target provider '{target_provider}' failed to bind; bound providers: {bound}",
            )
        in_meta = sess.get_inputs()[0]
        shape = [d if isinstance(d, int) and d > 0 else 1 for d in in_meta.shape]
        dtype = {"tensor(float)": np.float32}.get(in_meta.type)
        if dtype is None:
            return case("ERROR", False, f"Probe cannot feed input type {in_meta.type}")
        sess.run(None, {in_meta.name: np.zeros(shape, dtype=dtype)})
    except Exception as exc:
        exc_text = str(exc)
        named_gap = (
            target_provider == DEFAULT_TARGET_PROVIDER and TRANSNET_V2_SCATTERND_GAP in exc_text
        )
        if named_gap:
            return case(
                "PROVIDER_GAP",
                True,
                f"expected gap: {TRANSNET_V2_SCATTERND_GAP} (ORT 1.31.1 CUDA EP)",
            )
        return case("ERROR", False, f"Exception during probe inference: {exc}")
    return case(
        "OK",
        True,
        f"gap closed: {TRANSNET_V2_MODEL_ID} executes on {target_provider}; "
        "numerical parity not compared by this probe",
    )


def _stop_report(
    ref: str, target: str, available: bool, status: str, note: str, ok: bool = False
) -> ParityGateReport:
    """A report for a run that stopped before any model ran."""
    return ParityGateReport(
        reference_provider=ref,
        target_provider=target,
        target_available=available,
        ok=ok,
        status=status,
        note=note,
    )


def run_parity_gate(
    fp32_model: Path,
    fp16_model: Path,
    features_path: Path,
    transnet_model: Path = Path("model/tiny/transnet_v2.onnx"),
    ref_provider: str = DEFAULT_REF_PROVIDER,
    target_provider: str = DEFAULT_TARGET_PROVIDER,
    fp32_tol: float = DEFAULT_FP32_TOLERANCE,
    fp16_tol: float = DEFAULT_FP16_TOLERANCE,
    allow_missing_provider: bool = False,
) -> ParityGateReport:
    """Execute tiny AI cross-device parity checks across FP32 and FP16 cases."""
    available = ort.get_available_providers()
    target_available = target_provider in available
    if ref_provider not in available:
        return _stop_report(
            ref_provider,
            target_provider,
            target_available,
            "MISSING_REF_PROVIDER",
            f"Reference provider '{ref_provider}' not in available: {available}",
        )
    if not target_available:
        return _stop_report(
            ref_provider,
            target_provider,
            False,
            "MISSING_PROVIDER",
            f"Target provider '{target_provider}' is not available in onnxruntime "
            f"(available: {available})",
            ok=allow_missing_provider,
        )
    try:
        fp32_inputs = load_fp32_inputs(features_path)
    except (FileNotFoundError, ValueError, RuntimeError) as exc:
        return _stop_report(ref_provider, target_provider, True, "BAD_FIXTURE", str(exc))
    cases = _run_cases(
        (fp32_model, fp16_model, transnet_model),
        fp32_inputs,
        (fp32_tol, fp16_tol),
        ref_provider,
        target_provider,
    )
    all_ok = all(c.ok for c in cases)
    return ParityGateReport(
        reference_provider=ref_provider,
        target_provider=target_provider,
        target_available=True,
        ok=all_ok,
        status="PASS" if all_ok else "FAIL",
        cases=cases,
        tf32_pinned=DEFAULT_TARGET_PROVIDER in (ref_provider, target_provider),
    )


def _run_cases(
    models: tuple[Path, Path, Path],
    fp32_inputs: np.ndarray,
    tolerances: tuple[float, float],
    ref_provider: str,
    target_provider: str,
) -> list[CaseResult]:
    """The FP32 and FP16 numerical cases, then the transnet_v2 execute probe."""
    return [
        evaluate_case(
            "FP32 vmaf_tiny_v2",
            models[0],
            "fp32",
            tolerances[0],
            fp32_inputs,
            ref_provider,
            target_provider,
        ),
        evaluate_case(
            "FP16 smoke_fp16_v0",
            models[1],
            "fp16",
            tolerances[1],
            load_fp16_inputs(),
            ref_provider,
            target_provider,
        ),
        evaluate_provider_gap(models[2], target_provider),
    ]


def render_markdown(report: ParityGateReport) -> str:
    """Render human-readable Markdown summary table."""
    tf32_mode = (
        "use_tf32=0 pinned on every CUDAExecutionProvider session"
        if report.tf32_pinned
        else "not pinned (no CUDA session in this run)"
    )
    lines = [
        "# Tiny AI Cross-Device Parity Gate Report (T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25)",
        "",
        f"- **Reference Provider**: `{report.reference_provider}`",
        f"- **Target Provider**: `{report.target_provider}`",
        f"- **Target Available**: `{report.target_available}`",
        f"- **TF32**: {tf32_mode}",
        f"- **Verdict**: **{report.status}**",
    ]
    if report.note:
        lines.append(f"- **Note**: {report.note}")
    gaps = [c.name for c in report.cases if c.status == "PROVIDER_GAP"]
    if gaps:
        lines.append(f"- **Expected provider gaps**: {', '.join(gaps)}")
    lines.extend(
        [
            "",
            "| Case | Precision | Tolerance | Max |Δ| | Mean |Δ| | Status | Note |",
            "| :--- | :--- | :--- | :--- | :--- | :--- | :--- |",
        ]
    )
    for c in report.cases:
        lines.append(
            f"| {c.name} | {c.precision} | {c.tolerance:.1e} | {c.max_abs_error:.3e} | "
            f"{c.mean_abs_error:.3e} | **{c.status}** | {c.note or '-'} |"
        )
    lines.append("")
    return "\n".join(lines)


def render_json(report: ParityGateReport) -> dict[str, Any]:
    """Serialize report to structured dictionary."""
    data = asdict(report)
    data["schema_version"] = 1
    return data


_PATH_OPTIONS = (
    ("--fp32-model", "model/tiny/vmaf_tiny_v2.onnx", "FP32 ONNX model"),
    ("--fp16-model", "model/tiny/smoke_fp16_v0.onnx", "FP16 ONNX model"),
    (
        "--fp32-features",
        "ai/testdata/bisect/features.parquet",
        "canonical features parquet fixture",
    ),
    (
        "--transnet-model",
        "model/tiny/transnet_v2.onnx",
        "transnet_v2 ONNX model for the provider-gap probe",
    ),
)
_TOLERANCE_OPTIONS = (
    ("--fp32-tolerance", DEFAULT_FP32_TOLERANCE, "FP32"),
    ("--fp16-tolerance", DEFAULT_FP16_TOLERANCE, "FP16"),
)


def build_parser() -> argparse.ArgumentParser:
    """Construct CLI argument parser."""
    parser = argparse.ArgumentParser(
        description="Verify numerical parity for Tiny AI ONNX models across execution providers."
    )
    parser.add_argument("--reference-provider", default=DEFAULT_REF_PROVIDER)
    parser.add_argument("--target-provider", default=DEFAULT_TARGET_PROVIDER)
    for flag, path_default, what in _PATH_OPTIONS:
        parser.add_argument(
            flag, type=Path, default=Path(path_default), help=f"{what} (default: {path_default})"
        )
    for flag, tol_default, what in _TOLERANCE_OPTIONS:
        parser.add_argument(
            flag,
            type=float,
            default=tol_default,
            help=f"Maximum absolute error for {what} (default: {tol_default})",
        )
    parser.add_argument("--json-out", type=Path, default=None, help="Optional JSON report path")
    parser.add_argument("--md-out", type=Path, default=None, help="Optional Markdown report path")
    parser.add_argument(
        "--allow-missing-provider",
        action="store_true",
        help="Allow a missing target provider without failing closed (inspection only)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    parser = build_parser()
    args = parser.parse_args(argv)

    report = run_parity_gate(
        fp32_model=args.fp32_model,
        fp16_model=args.fp16_model,
        features_path=args.fp32_features,
        transnet_model=args.transnet_model,
        ref_provider=args.reference_provider,
        target_provider=args.target_provider,
        fp32_tol=args.fp32_tolerance,
        fp16_tol=args.fp16_tolerance,
        allow_missing_provider=args.allow_missing_provider,
    )

    md_text = render_markdown(report)
    print(md_text)

    if args.md_out:
        args.md_out.write_text(md_text, encoding="utf-8")

    if args.json_out:
        json_data = render_json(report)
        args.json_out.write_text(json.dumps(json_data, indent=2) + "\n", encoding="utf-8")

    if not report.ok:
        sys.stderr.write(f"::error::Tiny AI parity gate failed: {report.status} ({report.note})\n")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
