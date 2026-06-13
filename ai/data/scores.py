# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Distillation scores: per-frame ``vmaf_v0.6.1`` predictions as targets.

ADR-0203 picks distillation from ``vmaf_v0.6.1`` over Netflix's
published MOS subset because:

* the published MOS table covers fewer than half of the 70 distorted
  clips and is paywalled / subjective-test-version-dependent;
* the tiny model's job is to be a faster *replacement* for the SVR, not
  a more-accurate one — distillation gives the right loss signal.

The teacher score is computed with ``vmaf -m path=model/vmaf_v0.6.1.json``
once per clip; results land in the ``vmaf-tiny-ai`` cache (JSON of
``{"frames": [...], "pooled": float}``).
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .feature_extractor import _ensure_binary, default_vmaf_binary

DEFAULT_MODEL_PATH = Path("model/vmaf_v0.6.1.json")


@dataclass
class TeacherScores:
    """``vmaf_v0.6.1`` predictions for one clip."""

    per_frame: np.ndarray  # shape (n_frames,), float32
    pooled: float

    def to_jsonable(self) -> dict:
        return {
            "per_frame": self.per_frame.tolist(),
            "pooled": float(self.pooled),
        }

    @classmethod
    def from_jsonable(cls, payload: dict) -> "TeacherScores":
        per_frame = np.asarray(payload["per_frame"], dtype=np.float32)
        pooled = float(payload["pooled"])
        return cls(per_frame=per_frame, pooled=pooled)


def _model_path() -> Path:
    env = os.environ.get("VMAF_MODEL_PATH")
    return Path(env) if env else DEFAULT_MODEL_PATH


def _run_vmaf_score(
    binary: Path,
    ref: Path,
    dis: Path,
    width: int,
    height: int,
    *,
    pix_fmt: str,
    bitdepth: int,
    model: Path,
) -> dict:
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
        out_path = Path(tf.name)
    try:
        cmd = [
            str(binary),
            "-r",
            str(ref),
            "-d",
            str(dis),
            "-w",
            str(width),
            "-h",
            str(height),
            "-p",
            pix_fmt,
            "-b",
            str(bitdepth),
            "--json",
            "-o",
            str(out_path),
            "-m",
            f"path={model}",
        ]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        return json.loads(out_path.read_text())
    finally:
        out_path.unlink(missing_ok=True)


def teacher_scores(
    ref: Path,
    dis: Path,
    width: int,
    height: int,
    *,
    vmaf_binary: Path | None = None,
    pix_fmt: str = "420",
    bitdepth: int = 8,
    model: Path | None = None,
) -> TeacherScores:
    """Compute ``vmaf_v0.6.1`` predictions for ``(ref, dis)``.

    The ``"vmaf"`` metric is read out of each frame's ``metrics`` block;
    the ``pooled_metrics.vmaf.mean`` is preserved as the clip-level
    target. If pooling is missing, fall back to the per-frame mean.
    """
    binary = Path(vmaf_binary) if vmaf_binary is not None else default_vmaf_binary()
    _ensure_binary(binary)
    model_path = Path(model) if model is not None else _model_path()
    if not model_path.is_file():
        raise FileNotFoundError(
            f"VMAF model JSON not found at {model_path}. "
            "Set $VMAF_MODEL_PATH or pass model=… explicitly."
        )

    doc = _run_vmaf_score(
        binary,
        ref,
        dis,
        width,
        height,
        pix_fmt=pix_fmt,
        bitdepth=bitdepth,
        model=model_path,
    )
    per_frame: list[float] = []
    for frame in doc.get("frames", []):
        m = frame.get("metrics", {})
        v = m.get("vmaf")
        per_frame.append(float("nan") if v is None else float(v))
    arr = np.asarray(per_frame, dtype=np.float32)
    pooled_doc = doc.get("pooled_metrics", {}).get("vmaf", {})
    pooled = pooled_doc.get("mean") if isinstance(pooled_doc, dict) else None
    if pooled is None:
        pooled_val = float(np.nanmean(arr)) if arr.size else float("nan")
    else:
        pooled_val = float(pooled)
    return TeacherScores(per_frame=arr, pooled=pooled_val)
