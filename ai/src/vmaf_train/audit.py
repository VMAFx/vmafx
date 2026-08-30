# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Audit shipped tiny models for compatibility with libvmaf's feature contract.

Feature extractors in core/src/feature/ evolve independently from the
tiny models trained against their output vector. A new VIF scale, a
renamed motion feature, or a bit-depth change in the NR input can
silently break scoring. This audits every `.onnx` under a model dir and
flags anything whose sidecar or graph doesn't match the current C1/C2/C3
contract — so CI can gate deployment.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import onnx

from .features import FEATURE_COLUMNS

EXPECTED_FR_FEATURE_COUNT = len(FEATURE_COLUMNS)


@dataclass
class ModelAudit:
    model_path: Path
    sidecar_path: Path | None
    kind: str | None
    issues: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.issues


def _input_shape(model: onnx.ModelProto, name: str) -> tuple[int | str, ...] | None:
    for i in model.graph.input:
        if i.name != name:
            continue
        dims: list[int | str] = []
        for d in i.type.tensor_type.shape.dim:
            dims.append(d.dim_value if d.dim_value else (d.dim_param or "?"))
        return tuple(dims)
    return None


def audit_model(onnx_path: Path) -> ModelAudit:
    """Check one .onnx + sidecar pair against the current feature contract."""
    sidecar = onnx_path.with_suffix(".json")
    audit = ModelAudit(
        model_path=onnx_path,
        sidecar_path=sidecar if sidecar.is_file() else None,
        kind=None,
    )

    try:
        model = onnx.load(str(onnx_path))
    except Exception as exc:
        audit.issues.append(f"failed to load ONNX: {exc}")
        return audit

    graph_inputs = {i.name for i in model.graph.input}
    graph_outputs = {o.name for o in model.graph.output}

    if sidecar.is_file():
        meta = json.loads(sidecar.read_text())
        audit.kind = meta.get("kind")

        declared_inputs = set(meta.get("input_names") or [])
        declared_outputs = set(meta.get("output_names") or [])
        if declared_inputs != graph_inputs:
            audit.issues.append(
                f"sidecar input_names {sorted(declared_inputs)} != "
                f"graph inputs {sorted(graph_inputs)}"
            )
        if declared_outputs != graph_outputs:
            audit.issues.append(
                f"sidecar output_names {sorted(declared_outputs)} != "
                f"graph outputs {sorted(graph_outputs)}"
            )

        norm = meta.get("normalization") or {}
        mean = norm.get("mean")
        std = norm.get("std")
        if mean is not None and std is not None and len(mean) != len(std):
            audit.issues.append(
                f"normalization mean ({len(mean)}) / std ({len(std)}) length mismatch"
            )

        if audit.kind == "fr":
            _audit_fr(model, meta, audit)
        elif audit.kind == "nr":
            _audit_nr(model, audit)
        elif audit.kind == "filter":
            _audit_filter(model, audit)
        elif audit.kind is None:
            audit.issues.append("sidecar missing `kind` field")
        else:
            audit.issues.append(f"unknown kind `{audit.kind}` (expected fr|nr|filter)")
    else:
        audit.issues.append("no sidecar JSON next to .onnx — run `vmaf-train register`")

    return audit


def _audit_fr(model: onnx.ModelProto, meta: dict, audit: ModelAudit) -> None:
    inputs = meta.get("input_names") or []
    if len(inputs) != 1:
        audit.issues.append(f"fr model must have exactly 1 input, has {len(inputs)}")
        return
    shape = _input_shape(model, inputs[0])
    if shape is None or len(shape) < 2:
        audit.issues.append(f"fr input shape unavailable or too shallow: {shape}")
        return
    feat_dim = shape[-1]
    if isinstance(feat_dim, int) and feat_dim != EXPECTED_FR_FEATURE_COUNT:
        audit.issues.append(
            f"fr model expects {feat_dim} features but libvmaf's FEATURE_COLUMNS "
            f"currently emits {EXPECTED_FR_FEATURE_COUNT} "
            f"({', '.join(FEATURE_COLUMNS)})"
        )
    mean = (meta.get("normalization") or {}).get("mean")
    if isinstance(mean, list) and isinstance(feat_dim, int) and len(mean) != feat_dim:
        audit.issues.append(
            f"normalization mean length ({len(mean)}) != model input width ({feat_dim})"
        )


def _audit_nr(model: onnx.ModelProto, audit: ModelAudit) -> None:
    if len(model.graph.input) != 1:
        audit.issues.append(f"nr model must have 1 input, has {len(model.graph.input)}")
        return
    shape = _input_shape(model, model.graph.input[0].name)
    if shape is None or len(shape) != 4:
        audit.issues.append(f"nr input must be rank-4 (N,C,H,W); got {shape}")
        return
    channels = shape[1]
    if isinstance(channels, int) and channels not in (1, 3):
        audit.issues.append(f"nr input channel count must be 1 (Y) or 3 (YUV/RGB); got {channels}")


def _audit_filter(model: onnx.ModelProto, audit: ModelAudit) -> None:
    if len(model.graph.input) != 1 or len(model.graph.output) != 1:
        audit.issues.append("filter model must have exactly 1 input and 1 output")
        return
    ish = _input_shape(model, model.graph.input[0].name)
    osh = _input_shape_out(model, model.graph.output[0].name)
    if ish and osh and len(ish) != len(osh):
        audit.issues.append(f"filter rank mismatch: input {ish} vs output {osh}")


def _input_shape_out(model: onnx.ModelProto, name: str) -> tuple[int | str, ...] | None:
    for o in model.graph.output:
        if o.name != name:
            continue
        dims: list[int | str] = []
        for d in o.type.tensor_type.shape.dim:
            dims.append(d.dim_value if d.dim_value else (d.dim_param or "?"))
        return tuple(dims)
    return None


def audit_dir(model_dir: Path) -> list[ModelAudit]:
    return [audit_model(p) for p in sorted(model_dir.glob("**/*.onnx"))]


def render_table(audits: list[ModelAudit]) -> str:
    if not audits:
        return "no .onnx models found"
    lines = [f"{'model':<40} {'kind':<8} status"]
    lines.append("-" * 78)
    for a in audits:
        status = "OK" if a.ok else f"FAIL ({len(a.issues)})"
        lines.append(f"{a.model_path.name:<40} {(a.kind or '-'):<8} {status}")
        for issue in a.issues:
            lines.append(f"    - {issue}")
    return "\n".join(lines)
