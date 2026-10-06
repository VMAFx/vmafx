#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Write pre_blur_4x4.onnx, the learned-filter fixture of ``vmafx_filter_check.py legacy``.

One 3x3 binomial blur (a Conv, padding 1) on a [1,1,4,4] float plane: an NCHW
[1,1,H,W] model the pre-filter's luma path runs, which changes every sample
it reads, so a filter that skipped inference would not pass. The shipped
learned_filter_v1 declares a symbolic batch and 224x224 planes and is refused
by that path. Needs the ``onnx`` package:

    uv run --no-project --with onnx python ffmpeg-patches/test/make_pre_test_model.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

OUT = Path(__file__).resolve().parent / "pre_blur_4x4.onnx"


def main() -> int:
    kernel = np.array([[1, 2, 1], [2, 4, 2], [1, 2, 1]], dtype=np.float32) / 16.0
    weight = numpy_helper.from_array(kernel.reshape(1, 1, 3, 3), name="w")
    node = helper.make_node("Conv", ["x", "w"], ["y"], pads=[1, 1, 1, 1])
    graph = helper.make_graph(
        [node],
        "pre_blur_4x4",
        [helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, 4, 4])],
        [helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1, 4, 4])],
        [weight],
    )
    model = helper.make_model(
        graph, opset_imports=[helper.make_opsetid("", 17)], producer_name="vmafx-test"
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, sys.argv[1] if len(sys.argv) > 1 else OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
