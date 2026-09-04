# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Distillation scores: per-frame teacher VMAF predictions as targets.

The fork's default teacher model is single-sourced from
``core/include/libvmaf/model.h`` (``VMAF_DEFAULT_MODEL_VERSION``) and its
Python mirror ``tools/vmaf-tune/src/vmaftune/defaultmodel.py``
(``DEFAULT_MODEL``), pointing to ``vmaf_v1.0.16_3d0h`` (ADR-1168, ADR-1173).

The teacher score is computed via ``vmaf --model version=<teacher>`` (or
``-m path=<model_path>`` if an explicit file override is supplied) once per clip;
results land in the ``vmaf-tiny-ai`` cache (JSON of
``{"frames": [...], "pooled": float, "teacher_model": str}``).
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .feature_extractor import _ensure_binary, default_vmaf_binary

_REPO_ROOT = Path(__file__).resolve().parents[2]
_VMAFTUNE_SRC = _REPO_ROOT / "tools" / "vmaf-tune" / "src"
if str(_VMAFTUNE_SRC) not in sys.path:
    sys.path.insert(0, str(_VMAFTUNE_SRC))

from vmaftune.defaultmodel import DEFAULT_MODEL  # noqa: E402

DEFAULT_MODEL_PATH = _REPO_ROOT / "model" / "vmaf_v1.0.16" / "vmaf_v1.0.16_3d0h.json"


@dataclass(frozen=True)
class ResolvedTeacherModel:
    """Resolved model invocation argument and model identifier string."""

    arg: str  # e.g. "version=vmaf_v1.0.16_3d0h" or "path=/path/to/model.json"
    name: str  # e.g. "vmaf_v1.0.16_3d0h"
    is_path: bool = False
    resolved: str = ""
    path: Path | None = None

    def __iter__(self):
        return iter((self.arg, self.name))


def resolve_teacher_model(model: Path | str | None = None) -> ResolvedTeacherModel:
    """Resolve teacher model specification from argument, environment, or single-source default.

    Order of precedence:
    1. Explicit ``model`` argument (version string or file path).
    2. ``$VMAF_MODEL_PATH`` environment variable (file path override).
    3. Fork default model version from ``vmaftune.defaultmodel.DEFAULT_MODEL`` (ADR-1168).
    """
    if model is not None:
        s = str(model).strip()
        if s.startswith("version="):
            ver = s[len("version=") :]
            return ResolvedTeacherModel(arg=s, name=ver, is_path=False, resolved=ver)
        if s.startswith("path="):
            path_part = s[len("path=") :]
            p = Path(path_part)
            return ResolvedTeacherModel(arg=s, name=p.stem, is_path=True, resolved=str(p), path=p)
        p = Path(s)
        if p.is_file() or s.endswith(".json") or "/" in s:
            return ResolvedTeacherModel(
                arg=f"path={p}", name=p.stem, is_path=True, resolved=str(p), path=p
            )
        return ResolvedTeacherModel(arg=f"version={s}", name=s, is_path=False, resolved=s)

    env = os.environ.get("VMAF_MODEL_PATH")
    if env:
        p = Path(env)
        if not p.is_dir():
            return ResolvedTeacherModel(
                arg=f"path={p}", name=p.stem, is_path=True, resolved=str(p), path=p
            )

    return ResolvedTeacherModel(
        arg=f"version={DEFAULT_MODEL}",
        name=DEFAULT_MODEL,
        is_path=False,
        resolved=DEFAULT_MODEL,
    )


@dataclass
class TeacherScores:
    """Predictions from the VMAF teacher model for one clip."""

    per_frame: np.ndarray  # shape (n_frames,), float32
    pooled: float
    teacher_model: str = DEFAULT_MODEL

    def to_jsonable(self) -> dict:
        return {
            "per_frame": self.per_frame.tolist(),
            "pooled": float(self.pooled),
            "teacher_model": self.teacher_model,
        }

    @classmethod
    def from_jsonable(cls, payload: dict) -> TeacherScores:
        per_frame = np.asarray(payload["per_frame"], dtype=np.float32)
        pooled = float(payload["pooled"])
        teacher_model = payload.get("teacher_model", DEFAULT_MODEL)
        return cls(per_frame=per_frame, pooled=pooled, teacher_model=teacher_model)


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
    model: str | Path | None = None,
) -> dict:
    if model is None:
        resolved = resolve_teacher_model()
        model_arg = resolved.arg
    elif isinstance(model, str) and (model.startswith("version=") or model.startswith("path=")):
        model_arg = model
    else:
        resolved = resolve_teacher_model(model)
        model_arg = resolved.arg

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
            model_arg,
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
    model: Path | str | None = None,
) -> TeacherScores:
    """Compute teacher VMAF predictions for ``(ref, dis)``.

    The ``"vmaf"`` metric is read out of each frame's ``metrics`` block;
    the ``pooled_metrics.vmaf.mean`` is preserved as the clip-level
    target. If pooling is missing, fall back to the per-frame mean.
    """
    binary = Path(vmaf_binary) if vmaf_binary is not None else default_vmaf_binary()
    _ensure_binary(binary)
    resolved = resolve_teacher_model(model)
    if resolved.is_path:
        path_str = resolved.arg[5:] if resolved.arg.startswith("path=") else resolved.arg
        model_file = Path(path_str)
        if not model_file.is_file():
            raise FileNotFoundError(
                f"VMAF model JSON not found at {model_file}. "
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
        model=resolved.arg,
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
    return TeacherScores(per_frame=arr, pooled=pooled_val, teacher_model=resolved.name)
