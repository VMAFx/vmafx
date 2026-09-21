# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the per-EP quantisation report manifest."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

# pylint: disable=wrong-import-position
from ai.scripts import measure_quant_drop_per_ep


def _write_toy_registry(repo: Path) -> tuple[Path, Path, Path]:
    """Lay out a one-model tiny-model tree; return (script, model_dir, registry)."""
    script_path = repo / "ai" / "scripts" / "measure_quant_drop_per_ep.py"
    model_dir = repo / "model" / "tiny"
    registry = model_dir / "registry.json"
    script_path.parent.mkdir(parents=True)
    model_dir.mkdir(parents=True)
    script_path.write_text("# test entrypoint\n", encoding="utf-8")
    (model_dir / "toy.onnx").write_text("fp32", encoding="utf-8")
    (model_dir / "toy.int8.onnx").write_text("int8", encoding="utf-8")
    registry.write_text(
        json.dumps(
            {
                "models": [
                    {
                        "id": "toy_model",
                        "onnx": "toy.onnx",
                        "quant_mode": "dynamic",
                        "quant_accuracy_budget_plcc": 0.02,
                    }
                ]
            }
        ),
        encoding="utf-8",
    )
    return script_path, model_dir, registry


def _fake_per_ep_result(model_dir: Path) -> dict[str, Any]:
    """Return one passing CPU-EP measurement for the toy model."""
    return {
        "model_id": "toy_model",
        "fp32": str(model_dir / "toy.onnx"),
        "int8": str(model_dir / "toy.int8.onnx"),
        "budget": 0.02,
        "per_ep": {
            "cpu": {
                "status": "ok",
                "plcc": 0.999,
                "drop": 0.001,
                "worst_abs": 0.01,
                "wall_s": 0.1,
                "pass": True,
            }
        },
    }


def _assert_quant_ep_provenance(payload: dict[str, Any], out_dir: Path) -> None:
    """Assert the ADR-0661 provenance block of the per-EP quantisation report."""
    assert payload["models"][0]["model_id"] == "toy_model"
    provenance = payload["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/measure_quant_drop_per_ep.py"
    assert provenance["inputs"]["registry"]["kind"] == "file"
    assert provenance["inputs"]["extra_fp32"] == []
    assert provenance["outputs"]["json_report"]["path"] == str(out_dir / "results.json")
    assert provenance["outputs"]["markdown_report"]["path"] == str(out_dir / "results.md")
    assert provenance["args"]["eps"] == ["cpu"]


def test_quant_ep_report_records_run_provenance(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    repo = tmp_path / "repo"
    out_dir = tmp_path / "quant-eps"
    script_path, model_dir, registry = _write_toy_registry(repo)

    def fake_run_model(*_args: Any, **_kwargs: Any) -> Any:
        return _fake_per_ep_result(model_dir)

    monkeypatch.setattr(measure_quant_drop_per_ep, "REPO_ROOT", repo)
    monkeypatch.setattr(measure_quant_drop_per_ep, "SCRIPT_PATH", script_path)
    monkeypatch.setattr(measure_quant_drop_per_ep, "REGISTRY", registry)
    monkeypatch.setattr(measure_quant_drop_per_ep, "_run_model", fake_run_model)
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "measure_quant_drop_per_ep.py",
            "--eps",
            "cpu",
            "--out",
            str(out_dir),
            "--hw",
            "test-host",
        ],
    )

    assert measure_quant_drop_per_ep.main() == 0

    payload = json.loads((out_dir / "results.json").read_text(encoding="utf-8"))
    _assert_quant_ep_provenance(payload, out_dir)
    assert (out_dir / "results.md").is_file()
