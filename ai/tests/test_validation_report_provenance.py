# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Report-provenance tests for AI validation scripts."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
from typing import Any

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "ai" / "scripts"
REGISTRY_FAILURE_EXIT_CODE = 2
TIMEOUT_EXIT_CODE = 124


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
    # The validator reads every registered graph (ADR-1546): a real, tiny ONNX.
    onnx.write_bytes((REPO_ROOT / "model" / "tiny" / "smoke_v0.onnx").read_bytes())
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
                        "teacher": "none",
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


def test_check_registry_positive(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """A clean ``_check_registry`` call returns 0 and passes ``SCRIPT_TIMEOUT_S``."""
    mod = _load_script("validate_saliency_student")
    recorded: dict[str, Any] = {}

    def fake_call(cmd: list[str], **kwargs: Any) -> int:
        """Fake successful validator execution by recording options and returning zero."""
        recorded.update(kwargs)
        return 0

    monkeypatch.setattr(mod.subprocess, "call", fake_call)
    rc = mod._check_registry()
    captured = capsys.readouterr()
    assert rc == 0
    assert recorded.get("timeout") == mod.SCRIPT_TIMEOUT_S
    assert "rc=0" in captured.out


def test_check_registry_negative(monkeypatch: pytest.MonkeyPatch) -> None:
    """A failing ``_check_registry`` call returns the exit code of the registry script."""
    mod = _load_script("validate_saliency_student")

    def fake_call(cmd: list[str], **kwargs: Any) -> int:
        """Fake validator execution by returning the registry failure exit code."""
        return REGISTRY_FAILURE_EXIT_CODE

    monkeypatch.setattr(mod.subprocess, "call", fake_call)
    assert mod._check_registry() == REGISTRY_FAILURE_EXIT_CODE


def test_check_registry_boundary(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """A timed-out ``_check_registry`` call returns 124 and names ``SCRIPT_TIMEOUT_S``."""
    mod = _load_script("validate_saliency_student")

    def fake_call(cmd: list[str], **kwargs: Any) -> int:
        """Fake validator execution by raising TimeoutExpired with the script timeout."""
        raise mod.subprocess.TimeoutExpired(cmd, mod.SCRIPT_TIMEOUT_S)

    monkeypatch.setattr(mod.subprocess, "call", fake_call)
    rc = mod._check_registry()
    captured = capsys.readouterr()
    assert rc == TIMEOUT_EXIT_CODE
    assert "timed out after" in captured.out
    assert "SCRIPT_TIMEOUT_S" in captured.out
