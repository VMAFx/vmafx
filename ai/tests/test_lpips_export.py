# Copyright 2026 Lusoris and Claude (Anthropic)
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the legacy LPIPS exporter CLI glue."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

pytest.importorskip("onnx")
pytest.importorskip("torch")

from .conftest import load_ai_module


def _load_module():
    return load_ai_module("lpips_export", name="lpips_export_test")


def test_main_accepts_documented_out_and_sidecar_flags(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    mod = _load_module()
    onnx_path = tmp_path / "lpips_sq.onnx"
    sidecar_path = tmp_path / "lpips_sq.json"

    def fake_export(output: Path, opset: int) -> int:
        output.write_bytes(b"fake-onnx")
        return opset

    monkeypatch.setattr(mod, "_export", fake_export)
    monkeypatch.setattr(mod, "_parity_check", lambda *_args, **_kwargs: None)

    rc = mod.main(
        [
            "--out",
            str(onnx_path),
            "--sidecar",
            str(sidecar_path),
            "--opset",
            "17",
            "--skip-parity",
        ]
    )

    payload = json.loads(sidecar_path.read_text(encoding="utf-8"))
    assert rc == 0
    assert onnx_path.read_bytes() == b"fake-onnx"
    assert payload["onnx_opset"] == 17
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert payload["run_provenance"]["args"]["output"] == str(onnx_path)
    assert payload["run_provenance"]["args"]["sidecar"] == str(sidecar_path)
    assert payload["run_provenance"]["outputs"]["onnx"]["sha256"]
