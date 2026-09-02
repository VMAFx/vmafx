#!/usr/bin/env python3
"""Generate the tiny-AI attached multi-output ONNX fixture.

The fixture is intentionally tiny and deterministic: one luma NCHW input
feeds two scalar reductions. It exists only to gate the `vmaf_use_tiny_model`
attached-output routing path; it is not a quality model and is not listed in
the production registry.

Usage:  .venv/bin/python scripts/gen_multi_output_smoke_onnx.py
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import onnx
from onnx import TensorProto, helper

REPO_ROOT = Path(__file__).resolve().parent.parent
TINY_DIR = REPO_ROOT / "model" / "tiny"
ONNX_PATH = TINY_DIR / "smoke_multi_output_v0.onnx"
SIDECAR_PATH = TINY_DIR / "smoke_multi_output_v0.json"

OPSET = 17
INPUT_NAME = "luma"
OUTPUT_NAMES = ("mean_score", "peak_score")


def build_model() -> onnx.ModelProto:
    reduce_mean = helper.make_node(
        "ReduceMean",
        inputs=[INPUT_NAME],
        outputs=[OUTPUT_NAMES[0]],
        axes=[0, 1, 2, 3],
        keepdims=0,
        name="mean0",
    )
    reduce_max = helper.make_node(
        "ReduceMax",
        inputs=[INPUT_NAME],
        outputs=[OUTPUT_NAMES[1]],
        axes=[0, 1, 2, 3],
        keepdims=0,
        name="max0",
    )

    graph = helper.make_graph(
        nodes=[reduce_mean, reduce_max],
        name="vmaf_tiny_smoke_multi_output_v0",
        inputs=[helper.make_tensor_value_info(INPUT_NAME, TensorProto.FLOAT, [1, 1, 4, 4])],
        outputs=[
            helper.make_tensor_value_info(OUTPUT_NAMES[0], TensorProto.FLOAT, []),
            helper.make_tensor_value_info(OUTPUT_NAMES[1], TensorProto.FLOAT, []),
        ],
        initializer=[],
    )

    model = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", OPSET)],
        producer_name="vmaf-tiny-smoke-multi-output",
        producer_version="0",
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def write_sidecar(path: Path) -> None:
    payload = {
        "kind": "fr",
        "name": "multi_probe",
        "onnx_opset": OPSET,
        "input_name": INPUT_NAME,
        "output_names": list(OUTPUT_NAMES),
        "notes": "CI attached multi-output fixture - not a quality model.",
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def main() -> int:
    TINY_DIR.mkdir(parents=True, exist_ok=True)
    raw = build_model().SerializeToString(deterministic=True)
    ONNX_PATH.write_bytes(raw)
    write_sidecar(SIDECAR_PATH)
    print(f"wrote {ONNX_PATH} ({len(raw)} bytes, sha256={hashlib.sha256(raw).hexdigest()})")
    print(f"wrote {SIDECAR_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
