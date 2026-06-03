# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Evaluation harness for tiny-AI checkpoints.

Reports correlation metrics (PLCC, SROCC, KROCC), error (RMSE), and
single-clip inference latency vs the ``vmaf_v0.6.1`` teacher.

The harness deliberately accepts either:

* a NumPy array of predictions (for unit tests / when the model isn't
  ONNX yet), or
* an ONNX checkpoint exported by :mod:`ai.train.train`.

Output is a single JSON document at ``--out`` (default
``eval_report.json``) with the schema::

    {
      "n_samples": int,
      "plcc":  float,
      "srocc": float,
      "krocc": float,
      "rmse":  float,
      "latency_ms_p50_per_clip": float | null,
      "latency_ms_p95_per_clip": float | null,
      "model": str,
      "feature_dim": int,
    }
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence

import numpy as np


@dataclass
class EvalReport:
    n_samples: int
    plcc: float
    srocc: float
    krocc: float
    rmse: float
    latency_ms_p50_per_clip: float | None
    latency_ms_p95_per_clip: float | None
    model: str
    feature_dim: int

    def write(self, path: Path) -> Path:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(asdict(self), indent=2))
        return path


def correlation_metrics(pred: np.ndarray, target: np.ndarray) -> tuple[float, float, float, float]:
    """Compute ``(plcc, srocc, krocc, rmse)``.

    All inputs are 1-D float arrays. Empty inputs return four NaNs.
    """
    pred = np.asarray(pred, dtype=np.float64).reshape(-1)
    target = np.asarray(target, dtype=np.float64).reshape(-1)
    if pred.shape != target.shape:
        raise ValueError(f"shape mismatch: pred={pred.shape} target={target.shape}")
    if pred.size == 0:
        return float("nan"), float("nan"), float("nan"), float("nan")

    from scipy.stats import kendalltau, pearsonr, spearmanr

    plcc = float(pearsonr(pred, target).statistic)
    srocc = float(spearmanr(pred, target).statistic)
    krocc = float(kendalltau(pred, target).statistic)
    rmse = float(np.sqrt(np.mean((pred - target) ** 2)))
    return plcc, srocc, krocc, rmse


def _onnx_session(onnx_path: Path):  # type: ignore[no-untyped-def]
    import onnxruntime as ort

    return ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])


def _onnx_predict(session, features: np.ndarray) -> np.ndarray:  # type: ignore[no-untyped-def]
    input_name = session.get_inputs()[0].name
    out = session.run(None, {input_name: features.astype(np.float32)})[0]
    return np.asarray(out).reshape(-1)


def measure_latency_ms(
    session,  # type: ignore[no-untyped-def]
    *,
    feature_dim: int,
    n_warmup: int = 5,
    n_iters: int = 50,
    typical_clip_frames: int = 240,
) -> tuple[float, float]:
    """Median + p95 wall-time per clip on synthetic features.

    Uses ``typical_clip_frames`` rows of zero-features as a proxy for a
    240-frame 720p clip; the actual content does not matter for inference
    time on a feed-forward net. Returns ``(p50_ms, p95_ms)``.
    """
    rng = np.random.default_rng(0)
    batch = rng.standard_normal((typical_clip_frames, feature_dim)).astype(np.float32)
    input_name = session.get_inputs()[0].name
    for _ in range(n_warmup):
        session.run(None, {input_name: batch})
    samples_ms: list[float] = []
    for _ in range(n_iters):
        t0 = time.perf_counter()
        session.run(None, {input_name: batch})
        samples_ms.append((time.perf_counter() - t0) * 1e3)
    arr = np.asarray(samples_ms)
    return float(np.median(arr)), float(np.percentile(arr, 95))


def evaluate(
    *,
    features: np.ndarray,
    targets: np.ndarray,
    onnx_path: Path | None = None,
    predictions: np.ndarray | None = None,
    model_label: str = "tiny-fr",
    measure_latency: bool = True,
    out_path: Path | None = None,
) -> EvalReport:
    """Run correlation + (optional) latency evaluation.

    Pass either ``onnx_path`` (which is run through onnxruntime) **or**
    ``predictions`` (precomputed). One of the two is required.
    """
    if (onnx_path is None) == (predictions is None):
        raise ValueError("Pass exactly one of onnx_path or predictions.")
    feature_dim = int(features.shape[1]) if features.ndim == 2 else 0

    if onnx_path is not None:
        session = _onnx_session(Path(onnx_path))
        pred = _onnx_predict(session, features)
        latency: tuple[float, float] | tuple[None, None]
        if measure_latency:
            latency = measure_latency_ms(session, feature_dim=feature_dim)
        else:
            latency = (None, None)
    else:
        pred = np.asarray(predictions).reshape(-1)
        latency = (None, None)

    plcc, srocc, krocc, rmse = correlation_metrics(pred, targets)
    report = EvalReport(
        n_samples=int(pred.size),
        plcc=plcc,
        srocc=srocc,
        krocc=krocc,
        rmse=rmse,
        latency_ms_p50_per_clip=latency[0],
        latency_ms_p95_per_clip=latency[1],
        model=str(onnx_path) if onnx_path else model_label,
        feature_dim=feature_dim,
    )
    if out_path is not None:
        report.write(out_path)
    return report


def latencies_from_samples(samples_ms: Sequence[float]) -> tuple[float, float]:
    """Helper to expose median + p95 for callers that already timed."""
    arr = np.asarray(list(samples_ms), dtype=np.float64)
    if arr.size == 0:
        return float("nan"), float("nan")
    return float(np.median(arr)), float(np.percentile(arr, 95))
