# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run-provenance tests for DNN feature-model exporters."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from conftest import guarded_pytorch_lightning_import

with guarded_pytorch_lightning_import():
    import export_fastdvdnet_pre as fastdvdnet_real
    import export_fastdvdnet_pre_placeholder as fastdvdnet_placeholder
    import export_tiny_models as tiny_export
    import export_transnet_v2 as transnet_real
    import export_transnet_v2_placeholder as transnet_placeholder


def _provenance(entrypoint: str) -> dict[str, object]:
    return {
        "schema": "ai-run-provenance-v1",
        "entrypoint": {"path": entrypoint},
        "argv": ["--output", "fixture.onnx"],
        "args": {"output": "fixture.onnx"},
    }


def _read(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def test_export_tiny_models_sidecar_records_run_provenance(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    onnx_path = tmp_path / "nr_metric_v1.onnx"
    onnx_path.write_bytes(b"onnx")
    monkeypatch.setattr(tiny_export, "TINY_DIR", tmp_path)
    provenance = _provenance("ai/scripts/export_tiny_models.py")

    sidecar = tiny_export._write_sidecar(
        "nr_metric_v1",
        onnx_path,
        kind="nr",
        notes="fixture",
        run_provenance=provenance,
    )

    payload = _read(sidecar)
    assert payload["id"] == "nr_metric_v1"
    assert payload["sha256"] == tiny_export.sha256(onnx_path)
    assert payload["run_provenance"] == provenance


@pytest.mark.parametrize(
    ("module", "entrypoint", "model_id"),
    [
        (
            fastdvdnet_placeholder,
            "ai/scripts/export_fastdvdnet_pre_placeholder.py",
            "fastdvdnet_pre",
        ),
        (fastdvdnet_real, "ai/scripts/export_fastdvdnet_pre.py", "fastdvdnet_pre"),
        (
            transnet_placeholder,
            "ai/scripts/export_transnet_v2_placeholder.py",
            "transnet_v2",
        ),
        (transnet_real, "ai/scripts/export_transnet_v2.py", "transnet_v2"),
    ],
)
def test_feature_model_export_sidecar_records_run_provenance(
    tmp_path: Path, module, entrypoint: str, model_id: str
) -> None:
    onnx_path = tmp_path / f"{model_id}.onnx"
    onnx_path.write_bytes(b"onnx")
    provenance = _provenance(entrypoint)

    sidecar = module._write_sidecar(onnx_path, run_provenance=provenance)

    payload = _read(sidecar)
    assert payload["id"] == model_id
    assert payload["onnx"] == onnx_path.name
    assert payload["run_provenance"] == provenance
