# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage top-up for ``vmaf_train.op_allowlist``.

The existing ``test_op_allowlist.py`` covers the happy-path Loop scanner
and the FORBIDDEN_OP rejection. This file closes the residual branches:
``AllowlistReport.pretty()`` formatting, ``_collect_op_types`` recursion
into ``GRAPHS``-typed attributes (the rare multi-graph attribute used by
``Loop`` extensions), ``_constant_int64_value`` early returns for
non-Constant / wrong-dtype / no-value-attr nodes, ``Loop`` with no
inputs, and ``Loop.M`` traced to a non-Constant producer.
"""

from __future__ import annotations

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

from vmaf_train.op_allowlist import (
    AllowlistReport,
    _collect_loop_violations,
    _collect_op_types,
    _constant_int64_value,
    check_graph,
)

# ---------------------------------------------- AllowlistReport.pretty


def test_pretty_ok_report_mentions_op_count() -> None:
    """ok=True branch surfaces the distinct-op count (line 52)."""
    report = AllowlistReport(
        allowed=frozenset({"Relu", "Add"}),
        used=frozenset({"Relu", "Add"}),
        forbidden=frozenset(),
    )
    text = report.pretty()
    assert "allowlist OK" in text
    assert "2 distinct ops" in text


def test_pretty_failed_report_includes_loop_violations() -> None:
    """The loop-violation summary appears alongside the forbidden-ops summary."""
    report = AllowlistReport(
        allowed=frozenset({"Relu"}),
        used=frozenset({"Relu", "Scan"}),
        forbidden=frozenset({"Scan"}),
        loop_violations=("g::Loop(M=99999, max_trip_count=1024)",),
    )
    text = report.pretty()
    assert "allowlist FAIL" in text
    assert "1 forbidden op" in text
    assert "unbounded Loop" in text
    assert "M=99999" in text


# ----------------------------------- _constant_int64_value early returns


def test_constant_int64_value_non_constant_node_returns_none() -> None:
    """Non-Constant ``op_type`` short-circuits to None (line 94)."""
    node = helper.make_node("Relu", inputs=["x"], outputs=["y"])
    assert _constant_int64_value(node) is None


def test_constant_int64_value_non_value_attr_skipped() -> None:
    """An attribute named anything other than ``value`` is skipped (line 97)."""
    # Constant with `value_int` (the newer alternative encoding) — our
    # parser only inspects `value` and returns None when absent.
    node = helper.make_node(
        "Constant",
        inputs=[],
        outputs=["M"],
        name="c_value_int",
        value_int=5,
    )
    assert _constant_int64_value(node) is None


def test_constant_int64_value_wrong_dtype_returns_none() -> None:
    """A Constant whose ``value`` tensor is FLOAT (not INT64) returns None."""
    t = numpy_helper.from_array(np.array(8.0, dtype=np.float32), name="t")
    node = helper.make_node("Constant", inputs=[], outputs=["M"], value=t)
    assert _constant_int64_value(node) is None


def test_constant_int64_value_uses_int64_data_field() -> None:
    """The ``int64_data`` repeated-field encoding is supported (line 103)."""
    tensor = helper.make_tensor(
        name="M_t",
        data_type=TensorProto.INT64,
        dims=[],
        vals=[42],
    )
    # Force the int64_data encoding (helper.make_tensor uses int64_data
    # for INT64 when vals is a Python list, not raw_data).
    assert list(tensor.int64_data) == [42]
    node = helper.make_node("Constant", inputs=[], outputs=["M"], value=tensor)
    assert _constant_int64_value(node) == 42


def test_constant_int64_value_truncated_raw_data_returns_none() -> None:
    """A Constant whose ``raw_data`` is < 8 bytes falls through to None (lines 107-110)."""
    tensor = TensorProto()
    tensor.name = "M_t"
    tensor.data_type = TensorProto.INT64
    tensor.raw_data = b"\x01\x02"  # only 2 bytes — too short for int64
    node = helper.make_node("Constant", inputs=[], outputs=["M"], value=tensor)
    assert _constant_int64_value(node) is None


# -------------------- _collect_loop_violations edge cases


def test_loop_with_no_inputs_is_flagged() -> None:
    """A Loop node with zero inputs surfaces a ``no inputs`` violation (line 137)."""
    body = helper.make_graph(
        nodes=[helper.make_node("Relu", ["bx"], ["by"])],
        name="b",
        inputs=[helper.make_tensor_value_info("bx", TensorProto.FLOAT, [1])],
        outputs=[helper.make_tensor_value_info("by", TensorProto.FLOAT, [1])],
    )
    # Zero-input Loop — synthetic, not a valid runtime model, but the
    # scanner should still surface it.
    loop_node = helper.make_node("Loop", inputs=[], outputs=["y"], body=body)
    graph = helper.make_graph(
        nodes=[loop_node],
        name="g",
        inputs=[],
        outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])],
    )
    violations = _collect_loop_violations(graph)
    assert any("no inputs" in v for v in violations)


def test_loop_with_non_constant_producer_is_flagged() -> None:
    """Loop.M traces to a non-Constant op type → 'traces to ... not a scalar int64' (line 148)."""
    body = helper.make_graph(
        nodes=[helper.make_node("Relu", ["bx"], ["by"])],
        name="b",
        inputs=[helper.make_tensor_value_info("bx", TensorProto.FLOAT, [1])],
        outputs=[helper.make_tensor_value_info("by", TensorProto.FLOAT, [1])],
    )
    # Producer of "M" is an Identity, not a Constant — scanner cannot
    # statically bound it.
    identity_node = helper.make_node("Identity", inputs=["M_in"], outputs=["M"])
    loop_node = helper.make_node("Loop", inputs=["M", "cond", "x"], outputs=["y"], body=body)
    graph = helper.make_graph(
        nodes=[identity_node, loop_node],
        name="g",
        inputs=[
            helper.make_tensor_value_info("M_in", TensorProto.INT64, []),
            helper.make_tensor_value_info("cond", TensorProto.BOOL, []),
            helper.make_tensor_value_info("x", TensorProto.FLOAT, [1]),
        ],
        outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])],
    )
    violations = _collect_loop_violations(graph)
    assert any("traces to" in v and "Identity" in v for v in violations)


# ------------------- _collect_op_types / _collect_loop_violations GRAPHS branch


def _build_subgraph(op_type: str, name: str) -> onnx.GraphProto:
    return helper.make_graph(
        nodes=[helper.make_node(op_type, ["sub_in"], ["sub_out"])],
        name=name,
        inputs=[helper.make_tensor_value_info("sub_in", TensorProto.FLOAT, [1])],
        outputs=[helper.make_tensor_value_info("sub_out", TensorProto.FLOAT, [1])],
    )


def test_collect_op_types_recurses_into_graphs_attribute() -> None:
    """GRAPHS-typed attribute (line 76-78) is walked recursively."""
    sub_a = _build_subgraph("Relu", "sub_a")
    sub_b = _build_subgraph("Sigmoid", "sub_b")
    graphs_attr = helper.make_attribute("branches", [sub_a, sub_b])
    assert graphs_attr.type == onnx.AttributeProto.GRAPHS

    node = helper.make_node("CustomMultiBranch", inputs=["x"], outputs=["y"])
    node.attribute.append(graphs_attr)
    graph = helper.make_graph(
        nodes=[node],
        name="g",
        inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1])],
        outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])],
    )

    used = _collect_op_types(graph)
    # Outer + both subgraph ops.
    assert {"CustomMultiBranch", "Relu", "Sigmoid"} <= used


def test_collect_loop_violations_recurses_into_graphs_attribute() -> None:
    """``GRAPHS`` attribute (lines 167-168) recurses with an indexed scope tag."""
    inner_body = helper.make_graph(
        nodes=[helper.make_node("Relu", ["bx"], ["by"])],
        name="b",
        inputs=[helper.make_tensor_value_info("bx", TensorProto.FLOAT, [1])],
        outputs=[helper.make_tensor_value_info("by", TensorProto.FLOAT, [1])],
    )
    inner_loop = helper.make_node("Loop", inputs=[], outputs=["yi"], body=inner_body)
    sub = helper.make_graph(
        nodes=[inner_loop],
        name="sub_with_bad_loop",
        inputs=[],
        outputs=[helper.make_tensor_value_info("yi", TensorProto.FLOAT, [1])],
    )
    graphs_attr = helper.make_attribute("branches", [sub])

    outer_node = helper.make_node("CustomMultiBranch", inputs=["x"], outputs=["y"])
    outer_node.attribute.append(graphs_attr)
    graph = helper.make_graph(
        nodes=[outer_node],
        name="g",
        inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1])],
        outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])],
    )

    violations = _collect_loop_violations(graph)
    # Scope path should contain the indexed GRAPHS attribute name.
    assert any("branches[0]" in v for v in violations)


def test_check_graph_pretty_table_for_ok_model() -> None:
    """``check_graph`` + ``pretty`` for a fully-allowed model exercises the OK path."""
    x = helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 4])
    y = helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 4])
    model = helper.make_model(
        helper.make_graph([helper.make_node("Relu", ["x"], ["y"])], "g_ok", [x], [y]),
        opset_imports=[helper.make_opsetid("", 17)],
    )
    report = check_graph(model)
    assert report.ok
    assert "allowlist OK" in report.pretty()
