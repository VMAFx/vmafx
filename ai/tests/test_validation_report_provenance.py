# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Report-provenance tests for AI validation scripts."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "ai" / "scripts"


def _load_script(name: str):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS_DIR / f"{name}.py")
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_validate_model_registry_writes_report(tmp_path: Path) -> None:
    mod = _load_script("validate_model_registry")

    model_dir = tmp_path / "model" / "tiny"
    model_dir.mkdir(parents=True)
    onnx = model_dir / "smoke.onnx"
    onnx.write_bytes(b"not a real onnx; validator only hashes bytes")
    registry = model_dir / "registry.json"
    registry.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "models": [
                    {
                        "id": "smoke",
                        "kind": "fr",
                        "onnx": "smoke.onnx",
                        "sha256": hashlib.sha256(onnx.read_bytes()).hexdigest(),
                        "smoke": True,
                    }
                ],
            }
        )
        + "\n"
    )
    report = tmp_path / "registry_report.json"

    assert (
        mod.main(
            [
                str(registry),
                "--schema",
                str(REPO_ROOT / "model" / "tiny" / "registry.schema.json"),
                "--out-json",
                str(report),
            ]
        )
        == 0
    )
    payload = json.loads(report.read_text())
    assert payload["ok"] is True
    assert payload["model_count"] == 1
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"


def test_validate_saliency_student_writes_report(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    mod = _load_script("validate_saliency_student")
    report = tmp_path / "saliency_validation.json"
    onnx = tmp_path / "saliency.onnx"
    onnx.write_bytes(b"placeholder")

    monkeypatch.setattr(mod, "_check_allowlist", lambda _path: 0)
    monkeypatch.setattr(
        mod,
        "_check_parity",
        lambda _path, seed=0, threshold=1e-5: 0,
    )
    monkeypatch.setattr(mod, "_check_registry", lambda: 0)

    assert mod.main(["--onnx", str(onnx), "--out-json", str(report)]) == 0
    payload = json.loads(report.read_text())
    assert payload["ok"] is True
    assert payload["checks"] == {
        "op_allowlist": True,
        "pt_ort_parity": True,
        "registry": True,
    }
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
