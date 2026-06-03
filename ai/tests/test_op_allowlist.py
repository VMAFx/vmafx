# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Op-allowlist parser keeps Python and libvmaf's C source in lock-step."""

from __future__ import annotations

from pathlib import Path

import onnx
import pytest
from onnx import TensorProto, helper

from vmaf_train.op_allowlist import check_graph, check_model, load_allowlist


def test_allowlist_loads_nonempty() -> None:
    allowed = load_allowlist()
    # A few ops any realistic tiny model needs.
    for must_have in ("Conv", "Gemm", "Relu", "Add", "Reshape"):
        assert must_have in allowed, f"{must_have} missing — parser likely broken"


def test_loop_and_if_now_allowed() -> None:
    """ADR-0169 / T6-5: Loop + If joined the allowlist; their subgraph
    contents are validated recursively (see ``test_loop_body_*``)."""
    allowed = load_allowlist()
    assert "Loop" in allowed
    assert "If" in allowed


def test_scan_still_rejected() -> None:
    """Scan stays off-list — variant-typed input/output binding makes
    static bound-checking impractical (ADR-0169 § Alternatives)."""
    allowed = load_allowlist()
    assert "Scan" not in allowed


def test_resize_now_allowed() -> None:
    """ADR-0258 / T7-32: Resize joined the allowlist to unblock U-2-Net
    (PR #341) and the wider saliency / segmentation surface (mobilesal,
    BASNet, PiDiNet, FPN-style detectors)."""
    allowed = load_allowlist()
    assert "Resize" in allowed


def _build_model(op_type: str) -> onnx.ModelProto:
    x = helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 4])
    y = helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 4])
    node = helper.make_node(op_type, inputs=["x"], outputs=["y"])
    graph = helper.make_graph([node], f"g_{op_type}", [x], [y])
    return helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])


def test_allowed_op_passes() -> None:
    report = check_graph(_build_model("Relu"))
    assert report.ok
    assert "Relu" in report.used
    assert not report.forbidden


def test_forbidden_op_rejected() -> None:
    report = check_graph(_build_model("Scan"))
    assert not report.ok
    assert "Scan" in report.forbidden
    assert "forbidden" in report.pretty()


def _build_loop_with_body(body_op: str) -> onnx.ModelProto:
    """Build a ModelProto with a top-level Loop whose body subgraph
    contains a single ``body_op`` node. Used to exercise the recursive
    walker added in ADR-0169 / T6-5. The Loop's `M` input is wired to
    a `Constant` int64 of value 8 so the bounded-trip-count check from
    ADR-0171 / T6-5b passes."""
    import numpy as np
    from onnx import numpy_helper

    body_in = helper.make_tensor_value_info("body_x", TensorProto.FLOAT, [1, 4])
    body_out = helper.make_tensor_value_info("body_y", TensorProto.FLOAT, [1, 4])
    body_node = helper.make_node(body_op, inputs=["body_x"], outputs=["body_y"])
    body_graph = helper.make_graph([body_node], "loop_body", [body_in], [body_out])

    m_const = numpy_helper.from_array(np.array(8, dtype=np.int64), name="M_t")
    const_node = helper.make_node("Constant", inputs=[], outputs=["M"], value=m_const)
    loop_node = helper.make_node("Loop", inputs=["M", "cond", "x"], outputs=["y"], body=body_graph)

    cond_in = helper.make_tensor_value_info("cond", TensorProto.BOOL, [])
    x_in = helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 4])
    y_out = helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 4])
    graph = helper.make_graph([const_node, loop_node], "g_loop", [cond_in, x_in], [y_out])
    return helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])


def test_loop_body_with_allowed_op_passes() -> None:
    report = check_graph(_build_loop_with_body("Relu"))
    assert report.ok, report.pretty()
    assert "Loop" in report.used
    assert "Relu" in report.used


def test_loop_body_with_forbidden_op_rejected() -> None:
    report = check_graph(_build_loop_with_body("FakeOp"))
    assert not report.ok
    assert "FakeOp" in report.forbidden
    # The Loop wrapper itself should NOT be in `forbidden` — only the
    # nested op fails.
    assert "Loop" not in report.forbidden


def test_check_model_from_file(tmp_path: Path) -> None:
    model = _build_model("Sigmoid")
    p = tmp_path / "m.onnx"
    onnx.save(model, str(p))
    assert check_model(p).ok


def test_missing_source_raises(tmp_path: Path) -> None:
    missing = tmp_path / "nope.c"
    with pytest.raises(FileNotFoundError):
        load_allowlist(missing)


# ─────────────────────────────────────────────────────────────────────
# T6-5b / ADR-0171 — bounded Loop trip-count tests
# ─────────────────────────────────────────────────────────────────────


def _build_loop_with_const_trip(trip_count: int) -> onnx.ModelProto:
    """Build a ModelProto whose top-level Loop has its `M` input wired
    to a `Constant` int64 scalar of value ``trip_count``."""
    import numpy as np
    from onnx import numpy_helper

    body = helper.make_graph(
        nodes=[helper.make_node("Relu", ["body_x"], ["body_y"])],
        name="b",
        inputs=[helper.make_tensor_value_info("body_x", TensorProto.FLOAT, [1])],
        outputs=[helper.make_tensor_value_info("body_y", TensorProto.FLOAT, [1])],
    )
    m_const = numpy_helper.from_array(np.array(trip_count, dtype=np.int64), name="M_t")
    return helper.make_model(
        helper.make_graph(
            nodes=[
                helper.make_node("Constant", [], ["M"], value=m_const),
                helper.make_node("Loop", ["M", "cond", "x"], ["y"], body=body),
            ],
            name="g",
            inputs=[
                helper.make_tensor_value_info("cond", TensorProto.BOOL, []),
                helper.make_tensor_value_info("x", TensorProto.FLOAT, [1]),
            ],
            outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])],
        ),
        opset_imports=[helper.make_opsetid("", 17)],
    )


def test_loop_with_bounded_const_trip_passes() -> None:
    from vmaf_train.op_allowlist import MAX_LOOP_TRIP_COUNT

    report = check_graph(_build_loop_with_const_trip(MAX_LOOP_TRIP_COUNT // 2))
    assert report.ok, report.pretty()
    assert not report.loop_violations


def test_loop_with_too_large_const_trip_rejected() -> None:
    from vmaf_train.op_allowlist import MAX_LOOP_TRIP_COUNT

    report = check_graph(_build_loop_with_const_trip(MAX_LOOP_TRIP_COUNT * 4))
    assert not report.ok
    assert report.loop_violations
    assert "max_trip_count" in report.loop_violations[0]


def test_loop_with_negative_const_trip_rejected() -> None:
    report = check_graph(_build_loop_with_const_trip(-1))
    assert not report.ok
    assert report.loop_violations


def test_loop_with_dynamic_M_input_rejected() -> None:
    """Loop whose M input is a graph input (not a Constant) — the
    scanner can't statically bound it, so reject."""
    body = helper.make_graph(
        nodes=[helper.make_node("Relu", ["body_x"], ["body_y"])],
        name="b",
        inputs=[helper.make_tensor_value_info("body_x", TensorProto.FLOAT, [1])],
        outputs=[helper.make_tensor_value_info("body_y", TensorProto.FLOAT, [1])],
    )
    model = helper.make_model(
        helper.make_graph(
            nodes=[helper.make_node("Loop", ["M", "cond", "x"], ["y"], body=body)],
            name="g",
            inputs=[
                helper.make_tensor_value_info("M", TensorProto.INT64, []),
                helper.make_tensor_value_info("cond", TensorProto.BOOL, []),
                helper.make_tensor_value_info("x", TensorProto.FLOAT, [1]),
            ],
            outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])],
        ),
        opset_imports=[helper.make_opsetid("", 17)],
    )
    report = check_graph(model)
    assert not report.ok
    assert any("graph input" in v for v in report.loop_violations)


def test_caller_can_override_max_trip_count() -> None:
    """The MAX cap is overridable per call (operators with longer
    iterative pipelines can opt in to a higher bound)."""
    report = check_graph(_build_loop_with_const_trip(5000), max_loop_trip_count=10000)
    assert report.ok, report.pretty()
