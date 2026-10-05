# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary tests for the iterative tree walks (HISS-01).

Covers ``aiutils.tree_utils.map_tree`` and the callers that used to recurse:
``jsonl_utils._sanitize_nonfinite``, ``run_manifest.normalise_manifest_value``
and ``op_allowlist._collect_loop_violations`` / ``_collect_op_types``.
"""

from __future__ import annotations

import math
from pathlib import Path

import onnx
from onnx import TensorProto, helper

from aiutils.jsonl_utils import dumps_jsonl_row
from aiutils.run_manifest import normalise_manifest_value
from aiutils.tree_utils import map_tree
from vmaf_train.op_allowlist import _collect_loop_violations, _collect_op_types

_DEPTH = 5000  # far beyond the default interpreter recursion limit


def _is_dict(value: object) -> bool:
    return isinstance(value, dict)


def _is_list(value: object) -> bool:
    return isinstance(value, list)


def test_map_tree_preserves_key_and_item_order() -> None:
    src = {"b": [3, {"z": 1, "a": 2}], "a": 0}
    out = map_tree(src, lambda v: v + 1, is_mapping=_is_dict, is_sequence=_is_list)
    assert out == {"b": [4, {"z": 2, "a": 3}], "a": 1}
    assert list(out) == ["b", "a"]
    assert list(out["b"][1]) == ["z", "a"]


def test_map_tree_leaf_only_input_goes_through_leaf() -> None:
    assert map_tree(3, lambda v: v * 2) == 6
    assert map_tree([1], str) == "[1]"  # no container predicate: the list is a leaf


def test_map_tree_empty_containers_and_key_transform() -> None:
    out = map_tree({1: {}, 2: []}, repr, is_mapping=_is_dict, is_sequence=_is_list, key=str)
    assert out == {"1": {}, "2": []}


def test_map_tree_handles_depth_beyond_recursion_limit() -> None:
    src: list = []
    cursor = src
    for _ in range(_DEPTH):
        nxt: list = []
        cursor.append(nxt)
        cursor = nxt
    cursor.append(1)
    out = map_tree(src, lambda v: v, is_mapping=_is_dict, is_sequence=_is_list)
    depth = 0
    node = out
    while node and isinstance(node[0], list):
        node = node[0]
        depth += 1
    assert depth == _DEPTH
    assert node == [1]


def test_sanitize_nonfinite_replaces_nested_nan_and_inf() -> None:
    row = {"b": [math.nan, 1.0, {"c": math.inf}], "a": -math.inf}
    assert dumps_jsonl_row(row) == '{"a": null, "b": [null, 1.0, {"c": null}]}\n'


def test_normalise_manifest_value_sorts_and_stringifies() -> None:
    value = {2: Path("x"), 1: (math.nan, {"k": object}), "z": None}
    out = normalise_manifest_value(value)
    assert list(out) == ["1", "2", "z"]
    assert out["2"] == "x"
    assert out["1"][0] is None
    assert out["1"][1] == {"k": str(object)}


def _loop_node(name: str, m_input: str | None, body: onnx.GraphProto) -> onnx.NodeProto:
    inputs = [m_input] if m_input is not None else []
    return helper.make_node("Loop", inputs, [f"{name}_out"], name=name, body=body)


def _empty_graph(name: str, nodes: list[onnx.NodeProto]) -> onnx.GraphProto:
    return helper.make_graph(nodes, name, [], [])


def test_loop_violations_follow_document_order() -> None:
    """A subgraph's findings come right after its parent node, before later nodes."""
    inner_bad = _empty_graph("inner", [_loop_node("deep", None, _empty_graph("b0", []))])
    first = helper.make_node(
        "If", ["c"], ["o1"], name="first", then_branch=inner_bad, else_branch=_empty_graph("e", [])
    )
    second = _loop_node("second", "graph_input", _empty_graph("b1", []))
    graph = _empty_graph("top", [first, second])
    assert _collect_loop_violations(graph) == [
        "<top>::If.then_branch::Loop(no inputs)",
        "<top>::Loop(M='graph_input' is a graph input, not a Constant)",
    ]


def test_loop_violations_clean_graph_is_empty() -> None:
    const = helper.make_node(
        "Constant",
        [],
        ["m"],
        value=helper.make_tensor("v", TensorProto.INT64, [], [4]),
    )
    loop = _loop_node("ok", "m", _empty_graph("body", []))
    assert _collect_loop_violations(_empty_graph("top", [const, loop])) == []


def test_collect_op_types_survives_deep_nesting() -> None:
    graph = _empty_graph("leaf", [helper.make_node("Relu", ["x"], ["y"])])
    for i in range(25):
        node = helper.make_node(
            "If", ["c"], [f"o{i}"], then_branch=graph, else_branch=_empty_graph(f"e{i}", [])
        )
        graph = _empty_graph(f"g{i}", [node])
    assert _collect_op_types(graph) == {"If", "Relu"}
