# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Cross-backend (CPU/CUDA/OpenVINO/...) numerical-parity check for ONNX models.

Mirrors the /cross-backend-diff discipline we apply to VMAF scoring
(≤ 2 ULP in double precision) to tiny models: a model exported for
deployment must produce the same score on every execution provider, or
we risk a silent VMAF-point drift when the production server uses a
different provider than the validation lab.

The gate runs the model on the CPU provider (as reference) and every
other requested / available provider, then returns the maximum
|cpu - other| across the shared dataset. A CI check fails closed if the
max exceeds a threshold.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import onnxruntime as ort

DEFAULT_ATOL = 1e-3
CPU_PROVIDER = "CPUExecutionProvider"


@dataclass
class BackendComparison:
    provider: str
    max_abs_error: float
    mean_abs_error: float
    shape: tuple[int, ...]


@dataclass
class CrossBackendReport:
    model: Path
    atol: float
    reference_provider: str = CPU_PROVIDER
    comparisons: list[BackendComparison] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return all(c.max_abs_error <= self.atol for c in self.comparisons)

    def to_dict(self) -> dict:
        return {
            "model": str(self.model),
            "atol": self.atol,
            "reference_provider": self.reference_provider,
            "comparisons": [c.__dict__ | {"shape": list(c.shape)} for c in self.comparisons],
            "missing": list(self.missing),
            "ok": self.ok,
        }


def _load_features(path: Path, n_rows: int | None) -> np.ndarray:
    import pandas as pd

    from .features import FEATURE_COLUMNS

    df = pd.read_parquet(path)
    cols = [c for c in FEATURE_COLUMNS if c in df.columns]
    if not cols:
        raise ValueError(f"{path} has no FEATURE_COLUMNS; got {list(df.columns)}")
    x = df[cols].to_numpy(dtype=np.float32)
    if n_rows is not None:
        x = x[:n_rows]
    return x


def _infer_or_given_shape(model_path: Path, explicit: tuple[int, ...] | None) -> tuple[int, ...]:
    import onnx

    if explicit is not None:
        return explicit
    model = onnx.load(str(model_path))
    if not model.graph.input:
        raise ValueError(f"{model_path} has no inputs")
    return tuple(
        d.dim_value if d.dim_value else 4 for d in model.graph.input[0].type.tensor_type.shape.dim
    )


def compare_backends(
    model_path: Path,
    providers: list[str] | None = None,
    features: Path | None = None,
    shape: tuple[int, ...] | None = None,
    n_rows: int | None = 256,
    atol: float = DEFAULT_ATOL,
    seed: int = 0,
) -> CrossBackendReport:
    """Run @p model_path on CPU (reference) and each requested provider.

    If @p features is given, inputs come from the parquet feature cache
    (realistic distribution). Otherwise synthetic data of @p shape is
    used — useful for NR / filter models that don't consume the C1
    feature vector.
    """
    available = ort.get_available_providers()
    if CPU_PROVIDER not in available:  # pragma: no cover - always present
        raise RuntimeError("ORT has no CPU provider — cannot run parity check")

    requested = providers or [p for p in available if p != CPU_PROVIDER]
    report = CrossBackendReport(model=model_path, atol=atol)
    report.missing = [p for p in requested if p not in available]
    runnable = [p for p in requested if p in available]

    if features is not None:
        x = _load_features(features, n_rows)
    else:
        rng = np.random.default_rng(seed)
        real_shape = _infer_or_given_shape(model_path, shape)
        x = rng.standard_normal(real_shape, dtype=np.float32)

    cpu_sess = ort.InferenceSession(str(model_path), providers=[CPU_PROVIDER])
    cpu_input_name = cpu_sess.get_inputs()[0].name
    # ORT's session.run() returns a union (ndarray | OrtValue | SparseTensor |
    # list | dict); for a single dense FP regressor output we know it is
    # ``np.ndarray`` and narrow accordingly so downstream ``.astype`` resolves.
    cpu_out_raw = cpu_sess.run(None, {cpu_input_name: x})[0]
    assert isinstance(
        cpu_out_raw, np.ndarray
    ), f"unexpected ORT CPU output type {type(cpu_out_raw).__name__}"
    cpu_out: np.ndarray = cpu_out_raw

    for ep in runnable:
        sess = ort.InferenceSession(str(model_path), providers=[ep])
        out_raw = sess.run(None, {sess.get_inputs()[0].name: x})[0]
        assert isinstance(
            out_raw, np.ndarray
        ), f"unexpected ORT {ep} output type {type(out_raw).__name__}"
        out: np.ndarray = out_raw
        diff = np.abs(out.astype(np.float64) - cpu_out.astype(np.float64))
        report.comparisons.append(
            BackendComparison(
                provider=ep,
                max_abs_error=float(diff.max()) if diff.size else 0.0,
                mean_abs_error=float(diff.mean()) if diff.size else 0.0,
                shape=tuple(x.shape),
            )
        )
    return report


def render_table(report: CrossBackendReport) -> str:
    lines = [
        (
            f"model: {report.model.name}   reference: {report.reference_provider}   "
            f"atol: {report.atol:g}"
        ),
        f"{'provider':<32} {'max |Δ|':>12} {'mean |Δ|':>12} status",
        "-" * 72,
    ]
    if not report.comparisons:
        lines.append("(no alternate providers available — CPU-only install)")
    for c in report.comparisons:
        status = "OK" if c.max_abs_error <= report.atol else "FAIL"
        lines.append(
            f"{c.provider:<32} {c.max_abs_error:>12.3g} {c.mean_abs_error:>12.3g} {status}"
        )
    if report.missing:
        lines.append("")
        lines.append(f"requested but unavailable: {', '.join(report.missing)}")
    return "\n".join(lines)
