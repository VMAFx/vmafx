# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run-provenance tests for DNN feature-model exporters."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

pytest.importorskip("torch")

# Place repo root on sys.path BEFORE importing the conftest helper, since
# conftest.py lives next to this file and `from conftest import ...` works
# only when the tests dir is on the path (pytest sets this automatically).
REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

from conftest import requires_pytorch_lightning  # noqa: E402

requires_pytorch_lightning()

import export_fastdvdnet_pre as fastdvdnet_real  # noqa: E402
import export_fastdvdnet_pre_placeholder as fastdvdnet_placeholder  # noqa: E402
import export_tiny_models as tiny_export  # noqa: E402
import export_transnet_v2 as transnet_real  # noqa: E402
import export_transnet_v2_placeholder as transnet_placeholder  # noqa: E402


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
    # The sidecar records the opset the file imports (ADR-1546), so it must be
    # a real graph; the tracked smoke model is the smallest one.
    onnx_path.write_bytes((REPO_ROOT / "model" / "tiny" / "smoke_v0.onnx").read_bytes())
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
    assert payload["opset"] == tiny_export.read_signature(onnx_path).default_opset
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


@pytest.mark.parametrize("module", [fastdvdnet_placeholder, transnet_placeholder])
def test_placeholder_update_registry_raises_when_registry_missing(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, module
) -> None:
    missing = tmp_path / "registry.json"
    monkeypatch.setattr(module, "REGISTRY", missing)

    with pytest.raises(FileNotFoundError, match="missing"):
        module._update_registry(tmp_path / "model.onnx")


@pytest.mark.parametrize("module", [fastdvdnet_placeholder, transnet_placeholder])
def test_placeholder_main_returns_one_when_registry_missing(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    module,
) -> None:
    onnx_path = tmp_path / "model.onnx"
    missing = tmp_path / "registry.json"
    monkeypatch.setattr(module, "REGISTRY", missing)
    monkeypatch.setattr(module, "_export", lambda *a, **k: onnx_path.write_bytes(b"onnx"))
    monkeypatch.setattr(module, "_write_sidecar", lambda path, **k: path.with_suffix(".json"))

    assert module.main(["--output", str(onnx_path)]) == 1
    assert f"missing {missing}" in capsys.readouterr().err


@pytest.mark.parametrize("module", [fastdvdnet_placeholder, transnet_placeholder])
def test_placeholder_main_no_registry_returns_zero(module, tmp_path: Path) -> None:
    assert module.main(["--output", str(tmp_path / "m.onnx"), "--no-registry"]) == 0
