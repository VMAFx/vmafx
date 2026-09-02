# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Run-provenance tests for legacy AI evaluation reports."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from unittest.mock import MagicMock  # noqa: E402

from ai.scripts import eval_loso_3arch as eval_3arch  # noqa: E402
from ai.scripts import eval_loso_mlp_small as eval_mlp  # noqa: E402
from ai.scripts import eval_probabilistic_proxy as eval_prob  # noqa: E402


def _touch(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"stub")
    return path


def _assert_provenance(payload: dict[str, Any], entrypoint: str) -> None:
    provenance = payload["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == entrypoint


def test_loso_mlp_small_report_records_run_provenance(monkeypatch, tmp_path: Path) -> None:
    data_root = tmp_path / "netflix"
    data_root.mkdir()
    loso_dir = tmp_path / "loso"
    for clip in ("A", "B"):
        _touch(loso_dir / f"fold_{clip}" / "mlp_small_final.onnx")
    small = _touch(tmp_path / "vmaf_tiny_v1.onnx")
    medium = _touch(tmp_path / "vmaf_tiny_v1_medium.onnx")

    monkeypatch.setattr(eval_mlp, "CLIPS", ["A", "B"])
    monkeypatch.setattr(
        eval_mlp,
        "_load_clip",
        lambda _root, _clip: (
            np.zeros((3, 6), dtype=np.float32),
            np.asarray([80.0, 81.0, 82.0], dtype=np.float32),
        ),
    )
    monkeypatch.setattr(eval_mlp, "_load_session", lambda _path: object())
    monkeypatch.setattr(
        eval_mlp,
        "_eval",
        lambda _session, _x, y: {"n": len(y), "plcc": 0.99, "srocc": 0.98, "rmse": 0.1},
    )
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "eval_loso_mlp_small.py",
            "--data-root",
            str(data_root),
            "--loso-dir",
            str(loso_dir),
            "--mlp-small-baseline",
            str(small),
            "--mlp-medium-baseline",
            str(medium),
            "--out",
            str(tmp_path / "out"),
        ],
    )

    assert eval_mlp.main() == 0
    payload = json.loads((tmp_path / "out" / "loso_mlp_small_eval.json").read_text())
    _assert_provenance(payload, "ai/scripts/eval_loso_mlp_small.py")
    assert payload["run_provenance"]["inputs"]["mlp_small_baseline"]["kind"] == "file"
    assert payload["run_provenance"]["outputs"]["markdown_report"]["path"].endswith(
        "loso_mlp_small_eval.md"
    )


def test_loso_3arch_report_records_run_provenance(monkeypatch, tmp_path: Path) -> None:
    data_root = tmp_path / "netflix"
    data_root.mkdir()
    runs_dir = tmp_path / "training_runs"
    for clip in ("A", "B"):
        _touch(runs_dir / "loso_linear" / f"fold_{clip}" / "linear_final.onnx")

    monkeypatch.setattr(eval_3arch, "CLIPS", ["A", "B"])
    monkeypatch.setattr(eval_3arch, "ARCHS", ("linear",))
    monkeypatch.setattr(
        eval_3arch,
        "_load_clip",
        lambda _root, _clip: (
            np.zeros((3, 6), dtype=np.float32),
            np.asarray([80.0, 81.0, 82.0], dtype=np.float32),
        ),
    )
    monkeypatch.setattr(eval_3arch, "_load_session", lambda _path: object())
    monkeypatch.setattr(
        eval_3arch,
        "_eval",
        lambda _session, _x, y: {"n": len(y), "plcc": 0.99, "srocc": 0.98, "rmse": 0.1},
    )
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "eval_loso_3arch.py",
            "--data-root",
            str(data_root),
            "--training-runs-dir",
            str(runs_dir),
            "--out",
            str(tmp_path / "out"),
        ],
    )

    assert eval_3arch.main() == 0
    payload = json.loads((tmp_path / "out" / "loso_3arch_eval.json").read_text())
    _assert_provenance(payload, "ai/scripts/eval_loso_3arch.py")
    assert payload["run_provenance"]["inputs"]["training_runs_dir"]["kind"] == "directory"


def test_probabilistic_proxy_report_records_run_provenance(monkeypatch, tmp_path: Path) -> None:
    manifest_path = tmp_path / "ensemble.json"
    manifest = {
        "codec_vocab": ["x264", "x265"],
        "feature_mean": [0.0] * len(eval_prob.CANONICAL_6),
        "feature_std": [1.0] * len(eval_prob.CANONICAL_6),
    }
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    out = tmp_path / "prob.json"

    def _make_session_stub(num_codecs: int = 2):
        sess = MagicMock()
        codec_input = MagicMock()
        codec_input.name = "codec_onehot"
        codec_input.shape = [None, num_codecs]
        sess.get_inputs.return_value = [codec_input]
        return sess

    monkeypatch.setattr(
        eval_prob,
        "_load_ensemble",
        lambda _path: (manifest, [_make_session_stub(), _make_session_stub()]),
    )
    monkeypatch.setattr(
        eval_prob,
        "_predict_ensemble",
        lambda _sessions, features_norm, _codec_onehot: np.vstack(
            [
                np.linspace(70.0, 80.0, num=len(features_norm), dtype=np.float32),
                np.linspace(71.0, 81.0, num=len(features_norm), dtype=np.float32),
            ]
        ),
    )
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "eval_probabilistic_proxy.py",
            "--manifest",
            str(manifest_path),
            "--smoke",
            "--metrics-out",
            str(out),
        ],
    )

    assert eval_prob.main() == 0
    payload = json.loads(out.read_text(encoding="utf-8"))
    _assert_provenance(payload, "ai/scripts/eval_probabilistic_proxy.py")
    assert payload["run_provenance"]["inputs"]["manifest"]["kind"] == "file"
    assert payload["run_provenance"]["outputs"]["metrics_out"]["path"].endswith("prob.json")
