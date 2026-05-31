# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.measure_quant_drop` (T5-3b / ADR-0174).

Mocks ``onnxruntime.InferenceSession`` so we can drive the quant-drop
gate without any real ONNX artifacts on disk.
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
from unittest.mock import MagicMock, patch

import numpy as np
import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "measure_quant_drop.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("mqd_under_test", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


MQD = _load_module()


# ---------------------------------------------------------------------------
# _onnx_paths_for
# ---------------------------------------------------------------------------


def test_onnx_paths_for_derives_int8_sibling() -> None:
    fp32, int8 = MQD._onnx_paths_for({"onnx": "tinyA/model_v1.onnx"})
    assert fp32.name == "model_v1.onnx"
    assert int8.name == "model_v1.int8.onnx"
    assert fp32.parent == int8.parent


# ---------------------------------------------------------------------------
# _gate_one — skip / missing paths (no _measure required)
# ---------------------------------------------------------------------------


def test_gate_one_skips_fp32_entries() -> None:
    result = MQD._gate_one({"id": "model_x", "quant_mode": "fp32", "onnx": "x/y.onnx"})
    assert result["ok"] is True
    assert result["status"] == "skipped_fp32"
    assert result["id"] == "model_x"


def test_gate_one_skips_when_quant_mode_unset() -> None:
    # No 'quant_mode' key → defaults to fp32, returns skipped_fp32.
    result = MQD._gate_one({"id": "m", "onnx": "p/q.onnx"})
    assert result["status"] == "skipped_fp32"
    assert result["ok"] is True


def test_gate_one_marks_missing_models(tmp_path: Path) -> None:
    # Quant-mode != fp32 with no files on disk → status=missing_model, ok=False.
    entry = {"id": "missing_m", "quant_mode": "int8_dynamic", "onnx": "nope/missing.onnx"}
    result = MQD._gate_one(entry)
    assert result["status"] == "missing_model"
    assert result["ok"] is False
    assert result["fp32_exists"] is False
    assert result["int8_exists"] is False


# ---------------------------------------------------------------------------
# _measure — mocked InferenceSession
# ---------------------------------------------------------------------------


def _fake_session_factory(noise_scale: float = 0.0, seed: int = 0):
    """Build a callable returning an InferenceSession-like double."""

    rng = np.random.default_rng(seed)

    def _make(_path, providers):
        sess = MagicMock()
        inp = MagicMock()
        inp.name = "x"
        inp.shape = [1, 4]
        out = MagicMock()
        out.name = "y"

        def _run(_outputs, feeds):
            x = feeds["x"]
            base = x.sum(axis=-1, keepdims=True)
            if noise_scale > 0:
                base = base + rng.normal(0, noise_scale, base.shape).astype(np.float32)
            return [base.astype(np.float32)]

        sess.run.side_effect = _run
        sess.get_inputs.return_value = [inp]
        sess.get_outputs.return_value = [out]
        return sess

    return _make


def test_measure_returns_near_perfect_plcc_when_outputs_match(tmp_path: Path) -> None:
    fp32 = tmp_path / "fp32.onnx"
    int8 = tmp_path / "int8.onnx"
    fp32.write_bytes(b"\x00")
    int8.write_bytes(b"\x00")

    with patch.object(MQD, "_measure", wraps=MQD._measure):
        import onnxruntime as ort

        with patch.object(ort, "InferenceSession", side_effect=_fake_session_factory(0.0)):
            plcc, drop, worst = MQD._measure(fp32, int8)
    assert plcc == pytest.approx(1.0, abs=1e-6)
    assert drop == pytest.approx(0.0, abs=1e-6)
    assert worst == pytest.approx(0.0, abs=1e-6)


def test_measure_records_drop_when_outputs_diverge(tmp_path: Path) -> None:
    fp32 = tmp_path / "fp32.onnx"
    int8 = tmp_path / "int8.onnx"
    fp32.write_bytes(b"\x00")
    int8.write_bytes(b"\x00")

    import onnxruntime as ort

    # fp32 returns base, int8 returns base + noise.
    def factory(path, providers):
        if "int8" in str(path):
            return _fake_session_factory(noise_scale=0.5, seed=7)(path, providers)
        return _fake_session_factory(noise_scale=0.0, seed=0)(path, providers)

    with patch.object(ort, "InferenceSession", side_effect=factory):
        plcc, drop, worst = MQD._measure(fp32, int8)
    assert 0.0 <= plcc <= 1.0
    assert drop > 0.0
    assert worst > 0.0


# ---------------------------------------------------------------------------
# _gate_one — full integration with mocked _measure
# ---------------------------------------------------------------------------


def test_gate_one_pass_under_budget(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    # Pre-create matching fp32 + int8 files for the path probe.
    tiny_dir = MQD.REPO_ROOT / "model" / "tiny"
    # Use a real registry entry path schema, but patch _measure to fake values.
    entry = {
        "id": "stub",
        "quant_mode": "int8_dynamic",
        "onnx": "stub_v1.onnx",
        "quant_accuracy_budget_plcc": 0.05,
    }
    fp32 = tiny_dir / "stub_v1.onnx"
    int8 = tiny_dir / "stub_v1.int8.onnx"
    fp32.parent.mkdir(parents=True, exist_ok=True)
    fp32.write_bytes(b"\x00")
    int8.write_bytes(b"\x00")
    try:
        with patch.object(MQD, "_measure", return_value=(0.995, 0.005, 0.1)):
            res = MQD._gate_one(entry)
        assert res["ok"] is True
        assert res["status"] == "pass"
        assert res["plcc"] == pytest.approx(0.995)
        assert res["drop"] == pytest.approx(0.005)
    finally:
        fp32.unlink(missing_ok=True)
        int8.unlink(missing_ok=True)


def test_gate_one_fail_over_budget(tmp_path: Path) -> None:
    tiny_dir = MQD.REPO_ROOT / "model" / "tiny"
    entry = {
        "id": "stub_fail",
        "quant_mode": "int8_static",
        "onnx": "stub_fail.onnx",
        "quant_accuracy_budget_plcc": 0.001,
    }
    fp32 = tiny_dir / "stub_fail.onnx"
    int8 = tiny_dir / "stub_fail.int8.onnx"
    fp32.parent.mkdir(parents=True, exist_ok=True)
    fp32.write_bytes(b"\x00")
    int8.write_bytes(b"\x00")
    try:
        with patch.object(MQD, "_measure", return_value=(0.8, 0.2, 1.0)):
            res = MQD._gate_one(entry)
        assert res["ok"] is False
        assert res["status"] == "fail"
        assert res["drop"] > res["budget"]
    finally:
        fp32.unlink(missing_ok=True)
        int8.unlink(missing_ok=True)


# ---------------------------------------------------------------------------
# main — registry loading / argparse
# ---------------------------------------------------------------------------


def test_main_returns_2_when_registry_load_fails(monkeypatch: pytest.MonkeyPatch) -> None:
    def bust():
        raise OSError("synthetic")

    monkeypatch.setattr(MQD, "_load_registry", bust)
    rc = MQD.main(["--all"])
    assert rc == 2


def test_main_all_passes_when_every_entry_skipped(monkeypatch: pytest.MonkeyPatch) -> None:
    reg = {"models": [{"id": "x", "quant_mode": "fp32", "onnx": "p.onnx"}]}
    monkeypatch.setattr(MQD, "_load_registry", lambda: reg)
    rc = MQD.main(["--all"])
    assert rc == 0


def test_main_all_writes_out_json(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    reg = {"models": [{"id": "x", "quant_mode": "fp32", "onnx": "p.onnx"}]}
    monkeypatch.setattr(MQD, "_load_registry", lambda: reg)
    out = tmp_path / "report.json"
    rc = MQD.main(["--all", "--out-json", str(out)])
    assert rc == 0
    assert out.is_file()
    payload = json.loads(out.read_text())
    assert payload["gate_pass"] is True
    assert len(payload["models"]) == 1


def test_main_targeted_returns_2_for_unknown_path(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reg = {"models": [{"id": "x", "quant_mode": "fp32", "onnx": "stay.onnx"}]}
    monkeypatch.setattr(MQD, "_load_registry", lambda: reg)
    # An ONNX path outside model/tiny → returns 2 (input must live under model/tiny).
    rc = MQD.main([str(tmp_path / "outsider.onnx")])
    assert rc == 2
