# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""INT8 post-training quantization for shipped FR regressors.

Static PTQ via onnxruntime.quantization using a parquet feature cache as
the calibration set. We favour QDQ (QuantizeLinear / DequantizeLinear)
over QOperator format because:

  * QDQ lowers to standard ops most downstream EPs already understand
    (Conv + DequantizeLinear + QuantizeLinear vs. QLinearConv).
  * libvmaf's op allowlist does not include QLinear* variants. QDQ keeps
    the graph portable — the quantized model still uses Conv / Gemm /
    MatMul, which are already on the allowlist, plus QuantizeLinear /
    DequantizeLinear which this module adds.

After quantization we measure the drift between the fp32 and int8
outputs on a held-out slice of the calibration parquet. A 2-3 % RMSE
increase is expected and acceptable for C1 regressors; anything larger
is a red flag that calibration was too small / unrepresentative.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .features import FEATURE_COLUMNS

DEFAULT_CALIB_SAMPLES = 512
HELD_OUT_SAMPLES = 128


@dataclass
class QuantizationReport:
    fp32_path: Path
    int8_path: Path
    n_calibration: int
    n_held_out: int
    max_abs_error: float
    rmse: float
    fp32_bytes: int
    int8_bytes: int

    @property
    def compression_ratio(self) -> float:
        return self.fp32_bytes / max(self.int8_bytes, 1)

    def to_dict(self) -> dict:
        return {
            "fp32_path": str(self.fp32_path),
            "int8_path": str(self.int8_path),
            "n_calibration": self.n_calibration,
            "n_held_out": self.n_held_out,
            "max_abs_error": self.max_abs_error,
            "rmse": self.rmse,
            "fp32_bytes": self.fp32_bytes,
            "int8_bytes": self.int8_bytes,
            "compression_ratio": self.compression_ratio,
        }


def _load_calibration_features(parquet: Path, n_total: int) -> np.ndarray:
    import pandas as pd

    df = pd.read_parquet(parquet)
    cols = [c for c in FEATURE_COLUMNS if c in df.columns]
    if not cols:
        raise ValueError(f"{parquet} has no FEATURE_COLUMNS; got {list(df.columns)}")
    x = df[cols].to_numpy(dtype=np.float32)
    if len(x) < n_total + HELD_OUT_SAMPLES:
        raise ValueError(
            f"{parquet} has {len(x)} samples but need at least "
            f"{n_total + HELD_OUT_SAMPLES} for calibration + held-out eval"
        )
    # Use a fixed seed so the split is reproducible — matters for CI.
    rng = np.random.default_rng(0)
    idx = rng.permutation(len(x))
    return x[idx]


def _make_calibration_reader(features: np.ndarray, input_name: str, batch_size: int):
    """Build an onnxruntime CalibrationDataReader yielding feature batches."""
    from onnxruntime.quantization import CalibrationDataReader

    class _Reader(CalibrationDataReader):
        def __init__(self) -> None:
            self._iter = iter(
                features[i : i + batch_size] for i in range(0, len(features), batch_size)
            )

        def get_next(self) -> dict | None:
            try:
                batch = next(self._iter)
            except StopIteration:
                return None
            return {input_name: batch}

    return _Reader()


def quantize_int8(
    fp32_path: Path,
    int8_path: Path,
    calibration: Path,
    input_name: str = "features",
    n_calibration: int = DEFAULT_CALIB_SAMPLES,
    batch_size: int = 32,
) -> QuantizationReport:
    """Quantize @p fp32_path to INT8 using static PTQ, verify drift is bounded.

    Returns a :class:`QuantizationReport` with the numerical drift between
    the fp32 reference and INT8 output on a held-out slice of the
    calibration parquet. The caller owns the fail/pass decision — CI
    typically gates on ``rmse <= 1.0`` for C1 regressors (MOS is
    [0, 100], so a 1-unit RMSE is ~1 %).
    """
    import onnxruntime as ort
    from onnxruntime.quantization import QuantFormat, QuantType, quantize_static

    if not fp32_path.is_file():
        raise FileNotFoundError(f"fp32 ONNX not found: {fp32_path}")

    all_features = _load_calibration_features(calibration, n_calibration)
    calib = all_features[:n_calibration]
    held = all_features[n_calibration : n_calibration + HELD_OUT_SAMPLES]

    reader = _make_calibration_reader(calib, input_name, batch_size)
    int8_path.parent.mkdir(parents=True, exist_ok=True)
    quantize_static(
        str(fp32_path),
        str(int8_path),
        reader,
        quant_format=QuantFormat.QDQ,
        activation_type=QuantType.QInt8,
        weight_type=QuantType.QInt8,
        per_channel=True,
        reduce_range=True,
        op_types_to_quantize=["Gemm", "MatMul", "Conv"],
    )

    fp32_sess = ort.InferenceSession(str(fp32_path), providers=["CPUExecutionProvider"])
    int8_sess = ort.InferenceSession(str(int8_path), providers=["CPUExecutionProvider"])
    fp32_out = np.asarray(fp32_sess.run(None, {input_name: held})[0]).reshape(-1)
    int8_out = np.asarray(int8_sess.run(None, {input_name: held})[0]).reshape(-1)
    diff = fp32_out - int8_out
    return QuantizationReport(
        fp32_path=fp32_path,
        int8_path=int8_path,
        n_calibration=len(calib),
        n_held_out=len(held),
        max_abs_error=float(np.abs(diff).max()),
        rmse=float(np.sqrt((diff**2).mean())),
        fp32_bytes=fp32_path.stat().st_size,
        int8_bytes=int8_path.stat().st_size,
    )


def render_table(report: QuantizationReport) -> str:
    return "\n".join(
        [
            f"fp32: {report.fp32_path.name}  ({report.fp32_bytes / 1024:.1f} KiB)",
            f"int8: {report.int8_path.name}  ({report.int8_bytes / 1024:.1f} KiB)",
            f"compression: {report.compression_ratio:.2f}x",
            f"calibration samples: {report.n_calibration}  held-out: {report.n_held_out}",
            f"max |Δ|: {report.max_abs_error:.4g}  RMSE: {report.rmse:.4g}",
        ]
    )
