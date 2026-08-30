# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Feature-contract audit catches sidecar/graph drift before deploy."""

from __future__ import annotations

import json
from pathlib import Path

import onnx
from onnx import TensorProto, helper

from vmaf_train.audit import EXPECTED_FR_FEATURE_COUNT, audit_dir, audit_model, render_table


def _make_fr(path: Path, feature_count: int) -> None:
    x = helper.make_tensor_value_info("features", TensorProto.FLOAT, [None, feature_count])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, [None, 1])
    node = helper.make_node("Identity", ["features"], ["score"])
    graph = helper.make_graph([node], "fr", [x], [y])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


def _make_nr(path: Path, channels: int) -> None:
    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, [None, channels, 64, 64])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, [None, 1])
    node = helper.make_node("GlobalAveragePool", ["input"], ["score"])
    graph = helper.make_graph([node], "nr", [x], [y])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


def _write_sidecar(onnx_path: Path, **over) -> None:
    doc = {
        "schema_version": 1,
        "name": onnx_path.stem,
        "kind": "fr",
        "onnx_opset": 17,
        "input_names": ["features"],
        "output_names": ["score"],
        "normalization": {},
        "dataset": None,
        "train_commit": None,
        "train_config_hash": None,
        "parent_dataset_manifest": None,
        "expected_output_range": None,
        "license": None,
        "cosign_signature": None,
        "notes": None,
    }
    doc.update(over)
    onnx_path.with_suffix(".json").write_text(json.dumps(doc))


def test_fr_correct_shape_is_ok(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _make_fr(p, EXPECTED_FR_FEATURE_COUNT)
    _write_sidecar(p)
    assert audit_model(p).ok


def test_fr_wrong_feature_count_flagged(tmp_path: Path) -> None:
    p = tmp_path / "old.onnx"
    _make_fr(p, EXPECTED_FR_FEATURE_COUNT - 1)
    _write_sidecar(p)
    a = audit_model(p)
    assert not a.ok
    assert any("FEATURE_COLUMNS" in i for i in a.issues)


def test_missing_sidecar_flagged(tmp_path: Path) -> None:
    p = tmp_path / "naked.onnx"
    _make_fr(p, EXPECTED_FR_FEATURE_COUNT)
    a = audit_model(p)
    assert not a.ok
    assert any("sidecar" in i for i in a.issues)


def test_sidecar_graph_mismatch(tmp_path: Path) -> None:
    p = tmp_path / "bad.onnx"
    _make_fr(p, EXPECTED_FR_FEATURE_COUNT)
    _write_sidecar(p, input_names=["wrong_name"])
    a = audit_model(p)
    assert not a.ok
    assert any("input_names" in i for i in a.issues)


def test_normalization_length_drift(tmp_path: Path) -> None:
    p = tmp_path / "norm.onnx"
    _make_fr(p, EXPECTED_FR_FEATURE_COUNT)
    _write_sidecar(
        p,
        normalization={
            "mean": [0.0] * (EXPECTED_FR_FEATURE_COUNT - 1),
            "std": [1.0] * (EXPECTED_FR_FEATURE_COUNT - 1),
        },
    )
    a = audit_model(p)
    assert not a.ok
    assert any("normalization mean length" in i for i in a.issues)


def test_nr_invalid_channels_flagged(tmp_path: Path) -> None:
    p = tmp_path / "nr.onnx"
    _make_nr(p, channels=5)
    _write_sidecar(p, kind="nr", input_names=["input"])
    a = audit_model(p)
    assert not a.ok
    assert any("channel count" in i for i in a.issues)


def test_audit_dir_and_render(tmp_path: Path) -> None:
    good = tmp_path / "good.onnx"
    bad = tmp_path / "bad.onnx"
    _make_fr(good, EXPECTED_FR_FEATURE_COUNT)
    _write_sidecar(good)
    _make_fr(bad, EXPECTED_FR_FEATURE_COUNT + 2)
    _write_sidecar(bad)

    audits = audit_dir(tmp_path)
    assert len(audits) == 2
    table = render_table(audits)
    assert "good.onnx" in table and "bad.onnx" in table
    assert "FAIL" in table and "OK" in table


def test_render_empty() -> None:
    assert "no .onnx" in render_table([])
