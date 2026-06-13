# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Model registry — write/read sidecar metadata for shipped tiny models.

Each `.onnx` under `model/tiny/` gets a `<name>.json` sidecar recording:
  * kind (fr | nr | filter)
  * input_names, output_names
  * normalization (mean/std)
  * dataset, train_commit, train_config_hash
  * onnx_opset
  * expected_output_range (for runtime sanity bounds)
  * license
  * cosign_signature (filled in by release workflow)

Keeps a flat schema so the C loader can parse it with a minimal JSON reader.
"""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
from typing import Any

from pydantic import BaseModel, ConfigDict, Field, field_validator

SCHEMA_VERSION = 1
VALID_KINDS = {"fr", "nr", "filter"}


class ModelMetadata(BaseModel):
    """Sidecar JSON describing a shipped tiny model.

    Migrated from ``@dataclass`` to ``pydantic.BaseModel`` so the load
    path (``json.loads(...) -> ModelMetadata.model_validate``) rejects
    malformed sidecars with line-numbered errors instead of crashing
    inside Python's keyword-argument machinery. See ADR-0934.
    """

    model_config = ConfigDict(
        extra="forbid",
        validate_assignment=True,
    )

    schema_version: int
    name: str
    kind: str
    onnx_opset: int
    input_names: list[str]
    output_names: list[str]
    normalization: dict[str, list[float]] = Field(default_factory=dict)
    dataset: str | None = None
    train_commit: str | None = None
    train_config_hash: str | None = None
    parent_dataset_manifest: str | None = None
    expected_output_range: list[float] | None = None
    license: str | None = None
    cosign_signature: str | None = None
    notes: str | None = None

    @field_validator("kind")
    @classmethod
    def _valid_kind(cls, v: str) -> str:
        if v not in VALID_KINDS:
            raise ValueError(f"kind must be one of {sorted(VALID_KINDS)}; got {v!r}")
        return v

    def to_json(self) -> str:
        # ``mode='json'`` ensures Path / non-trivial types render as
        # JSON-native primitives; sort_keys + indent match the prior
        # dataclass-asdict output so the on-disk diff is empty.
        return json.dumps(self.model_dump(mode="json"), indent=2, sort_keys=True) + "\n"


def _hash_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(64 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def compute_config_hash(config_path: Path, manifest_path: Path | None = None) -> str:
    h = hashlib.sha256()
    h.update(config_path.read_bytes())
    if manifest_path is not None and manifest_path.exists():
        h.update(b"\x00")
        h.update(manifest_path.read_bytes())
    return h.hexdigest()


def register(
    onnx_path: Path,
    kind: str,
    dataset: str | None = None,
    license_: str | None = None,
    train_commit: str | None = None,
    train_config: Path | None = None,
    manifest: Path | None = None,
    normalization: dict[str, list[float]] | None = None,
    expected_output_range: tuple[float, float] | None = None,
    notes: str | None = None,
) -> Path:
    if kind not in VALID_KINDS:
        raise ValueError(f"kind must be one of {sorted(VALID_KINDS)}")
    import onnx

    model = onnx.load(str(onnx_path))
    onnx.checker.check_model(model)
    opset = max(o.version for o in model.opset_import) if model.opset_import else 0
    input_names = [i.name for i in model.graph.input]
    output_names = [o.name for o in model.graph.output]

    cfg_hash = compute_config_hash(train_config, manifest) if train_config else None

    meta = ModelMetadata(
        schema_version=SCHEMA_VERSION,
        name=onnx_path.stem,
        kind=kind,
        onnx_opset=opset,
        input_names=input_names,
        output_names=output_names,
        normalization=normalization or {},
        dataset=dataset,
        train_commit=train_commit,
        train_config_hash=cfg_hash,
        parent_dataset_manifest=_hash_file(manifest) if manifest else None,
        expected_output_range=list(expected_output_range) if expected_output_range else None,
        license=license_,
        notes=notes,
    )

    sidecar = onnx_path.with_suffix(".json")
    sidecar.write_text(meta.to_json())
    return sidecar


def load(sidecar_path: Path) -> ModelMetadata:
    doc: dict[str, Any] = json.loads(sidecar_path.read_text())
    return ModelMetadata.model_validate(doc)


def _sanitize_nonfinite(obj: Any) -> Any:
    """Recursively replace non-finite floats (NaN, Infinity) with None.

    Standard JSON does not support NaN or Infinity; replacing with null
    keeps the document valid while preserving all other numeric fields.
    """
    if isinstance(obj, float) and not math.isfinite(obj):
        return None
    if isinstance(obj, dict):
        return {k: _sanitize_nonfinite(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_sanitize_nonfinite(v) for v in obj]
    return obj


def dumps_registry_json(payload: dict, **kwargs: Any) -> str:
    """Serialise a registry payload to a pretty-printed, non-finite-safe JSON string.

    NaN and Infinity values inside *payload* are replaced with ``null`` so the
    output is valid RFC 8259 JSON.  The result is sorted and indented for
    human readability.  It does NOT end with a newline — use
    :func:`write_registry_json` when writing to a file.

    Args:
        payload:  The dict to serialise (e.g. ``{"models": [...]}``)
        **kwargs: Extra keyword arguments forwarded to :func:`json.dumps`.

    Returns:
        A JSON string (no trailing newline).
    """
    kwargs.setdefault("indent", 2)
    kwargs.setdefault("sort_keys", True)
    return json.dumps(_sanitize_nonfinite(payload), **kwargs)


def write_registry_json(path: Path, payload: dict, **kwargs: Any) -> None:
    """Write *payload* as pretty-printed, newline-terminated JSON to *path*.

    Convenience wrapper around :func:`dumps_registry_json` that appends a
    trailing newline (POSIX convention) and writes atomically via
    :meth:`Path.write_text`.

    Args:
        path:    Destination file path.
        payload: The dict to serialise.
        **kwargs: Forwarded to :func:`dumps_registry_json` / :func:`json.dumps`.
    """
    path.write_text(dumps_registry_json(payload, **kwargs) + "\n", encoding="utf-8")
