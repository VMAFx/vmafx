# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage top-up for ``vmaf_train.audit``.

The existing ``test_audit.py`` covers FR / NR happy + drift paths. This
file closes the residual branches: corrupt-ONNX load failure, normalization
mean/std length mismatch, unknown sidecar ``kind``, missing ``kind`` field,
NR rank-mismatch / extra-input flagged paths, the ``filter`` kind audit,
and FR ``input_names`` mismatch.
"""

from __future__ import annotations

import json
from pathlib import Path

import onnx
from onnx import TensorProto, helper

from vmaf_train.audit import EXPECTED_FR_FEATURE_COUNT, audit_model

# ---------------------------------------------- shared helpers


def _write_sidecar(onnx_path: Path, **over) -> None:
    doc = {
        "schema_version": 1,
        "name": onnx_path.stem,
        "kind": "fr",
        "onnx_opset": 17,
        "input_names": ["features"],
        "output_names": ["score"],
        "normalization": {},
    }
    doc.update(over)
    onnx_path.with_suffix(".json").write_text(json.dumps(doc))


def _make_fr(path: Path, feature_count: int) -> None:
    x = helper.make_tensor_value_info("features", TensorProto.FLOAT, [None, feature_count])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, [None, 1])
    node = helper.make_node("Identity", ["features"], ["score"])
    graph = helper.make_graph([node], "fr", [x], [y])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


# ------------------------------------- corrupt ONNX load failure


def test_corrupt_onnx_load_failure_flagged(tmp_path: Path) -> None:
    """A non-ONNX file under ``.onnx`` triggers the load-failure issue."""
    p = tmp_path / "corrupt.onnx"
    p.write_bytes(b"this is not an onnx file")
    _write_sidecar(p)

    a = audit_model(p)

    assert not a.ok
    assert any("failed to load ONNX" in i for i in a.issues)


# ------------------------------ normalization mean/std length mismatch


def test_normalization_mean_std_length_mismatch_flagged(tmp_path: Path) -> None:
    """mean and std arrays of different length surface a dedicated issue."""
    p = tmp_path / "len.onnx"
    _make_fr(p, EXPECTED_FR_FEATURE_COUNT)
    _write_sidecar(
        p,
        normalization={
            "mean": [0.0] * EXPECTED_FR_FEATURE_COUNT,
            "std": [1.0] * (EXPECTED_FR_FEATURE_COUNT - 2),
        },
    )

    a = audit_model(p)

    assert not a.ok
    assert any("mean" in i and "std" in i and "length mismatch" in i for i in a.issues)


# ----------------------- unknown / missing sidecar kind branches


def test_unknown_kind_flagged(tmp_path: Path) -> None:
    """An unrecognised ``kind`` value (not fr/nr/filter) is flagged."""
    p = tmp_path / "weird.onnx"
    _make_fr(p, EXPECTED_FR_FEATURE_COUNT)
    _write_sidecar(p, kind="hyperextractor")

    a = audit_model(p)

    assert not a.ok
    assert any("unknown kind" in i for i in a.issues)


def test_missing_kind_field_flagged(tmp_path: Path) -> None:
    """A sidecar without ``kind`` (or kind=null) surfaces 'sidecar missing `kind`'."""
    p = tmp_path / "nokind.onnx"
    _make_fr(p, EXPECTED_FR_FEATURE_COUNT)
    _write_sidecar(p, kind=None)

    a = audit_model(p)

    assert not a.ok
    assert any("missing `kind`" in i for i in a.issues)


# ------------------------------- FR sidecar input-count mismatch


def test_fr_with_zero_input_names_flagged(tmp_path: Path) -> None:
    """FR model declared with 0 input_names fails the ``len(inputs) != 1`` guard."""
    p = tmp_path / "fr_zero.onnx"
    _make_fr(p, EXPECTED_FR_FEATURE_COUNT)
    # Empty input_names list — also forces the graph-vs-sidecar mismatch check.
    _write_sidecar(p, input_names=[])

    a = audit_model(p)

    assert not a.ok
    assert any("fr model must have exactly 1 input" in i for i in a.issues)


# --------------------------------------- NR rank-mismatch path


def test_nr_input_not_rank_4_flagged(tmp_path: Path) -> None:
    """NR sidecar declared with a rank-2 input fails the rank-4 guard."""
    p = tmp_path / "nr_flat.onnx"
    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, [None, 16])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, [None, 1])
    node = helper.make_node("Identity", ["input"], ["score"])
    graph = helper.make_graph([node], "nr_flat", [x], [y])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(p))
    _write_sidecar(p, kind="nr", input_names=["input"])

    a = audit_model(p)

    assert not a.ok
    assert any("must be rank-4" in i for i in a.issues)


def test_nr_with_two_inputs_flagged(tmp_path: Path) -> None:
    """NR model with two graph inputs fails the ``1 input`` guard."""
    p = tmp_path / "nr_two.onnx"
    x1 = helper.make_tensor_value_info("input", TensorProto.FLOAT, [None, 1, 64, 64])
    x2 = helper.make_tensor_value_info("aux", TensorProto.FLOAT, [None, 1])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, [None, 1])
    node = helper.make_node("Identity", ["input"], ["score"])
    graph = helper.make_graph([node], "nr_two", [x1, x2], [y])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(p))
    _write_sidecar(p, kind="nr", input_names=["input", "aux"])

    a = audit_model(p)

    assert not a.ok
    assert any("must have 1 input" in i for i in a.issues)


# -------------------------------- filter audit happy + drift


def _make_filter(path: Path, in_dims: list[int], out_dims: list[int]) -> None:
    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, in_dims)
    y = helper.make_tensor_value_info("output", TensorProto.FLOAT, out_dims)
    node = helper.make_node("Identity", ["input"], ["output"])
    graph = helper.make_graph([node], "f", [x], [y])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


def test_filter_kind_matching_rank_is_ok(tmp_path: Path) -> None:
    """Filter model with matching input/output rank passes."""
    p = tmp_path / "filter_ok.onnx"
    _make_filter(p, [1, 3, 64, 64], [1, 3, 64, 64])
    _write_sidecar(p, kind="filter", input_names=["input"], output_names=["output"])

    a = audit_model(p)

    assert a.ok, a.issues
    assert a.kind == "filter"


def test_filter_rank_mismatch_flagged(tmp_path: Path) -> None:
    """Filter model with input/output rank mismatch surfaces a dedicated issue."""
    p = tmp_path / "filter_bad.onnx"
    _make_filter(p, [1, 3, 64, 64], [1, 3])
    _write_sidecar(p, kind="filter", input_names=["input"], output_names=["output"])

    a = audit_model(p)

    assert not a.ok
    assert any("rank mismatch" in i for i in a.issues)


def test_filter_multi_output_flagged(tmp_path: Path) -> None:
    """Filter model with two outputs fails the ``1 input and 1 output`` guard."""
    p = tmp_path / "filter_two.onnx"
    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, 64, 64])
    y1 = helper.make_tensor_value_info("o1", TensorProto.FLOAT, [1, 3, 64, 64])
    y2 = helper.make_tensor_value_info("o2", TensorProto.FLOAT, [1, 3, 64, 64])
    n1 = helper.make_node("Identity", ["input"], ["o1"])
    n2 = helper.make_node("Identity", ["input"], ["o2"])
    graph = helper.make_graph([n1, n2], "f", [x], [y1, y2])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(p))
    _write_sidecar(p, kind="filter", input_names=["input"], output_names=["o1", "o2"])

    a = audit_model(p)

    assert not a.ok
    assert any("exactly 1 input and 1 output" in i for i in a.issues)


# ------------------- FR normalization mean length != model input width


def test_fr_normalization_mean_length_mismatches_input(tmp_path: Path) -> None:
    """FR mean array shorter than feature dim is flagged separately from the
    feature-count check (lines 122-126)."""
    p = tmp_path / "fr_mean.onnx"
    _make_fr(p, EXPECTED_FR_FEATURE_COUNT)
    _write_sidecar(
        p,
        normalization={
            "mean": [0.0] * (EXPECTED_FR_FEATURE_COUNT - 2),
            "std": [1.0] * (EXPECTED_FR_FEATURE_COUNT - 2),
        },
    )

    a = audit_model(p)

    assert not a.ok
    assert any("normalization mean length" in i and "input width" in i for i in a.issues)
