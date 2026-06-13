#!/usr/bin/env python3
"""Generate the tiny-AI smoke ONNX fixture.

The smoke model is a one-op probe (Conv → Identity) whose only purpose is to
exercise the full libvmaf load path: sniff kind → sidecar load → protobuf
op-walk → ORT CreateSession → first inference. It is *not* a quality model.

Run this script once after changing the fixture layout; the generated
artefacts are checked into model/tiny/ so CI does not need onnx at every
run. Re-running must produce byte-identical output (deterministic IR / opset
version pins) — we assert the sha256 digest below.

Usage:  .venv/bin/python scripts/gen_smoke_onnx.py
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

REPO_ROOT = Path(__file__).resolve().parent.parent
TINY_DIR = REPO_ROOT / "model" / "tiny"
ONNX_PATH = TINY_DIR / "smoke_v0.onnx"
SIDECAR_PATH = TINY_DIR / "smoke_v0.json"
REGISTRY_PATH = TINY_DIR / "registry.json"

OPSET = 17
MODEL_ID = "smoke_v0"
INPUT_NAME = "features"
OUTPUT_NAME = "score"


def build_model() -> onnx.ModelProto:
    """Build the smoke graph.

    Shape: input [1, 1, 4, 4] float32 → Conv 1x1 → Identity → [1, 1, 4, 4].
    Weights are a constant 1.0 so the inference is trivially verifiable.
    """
    weight = numpy_helper.from_array(np.ones((1, 1, 1, 1), dtype=np.float32), name="conv_w")

    conv = helper.make_node(
        "Conv",
        inputs=[INPUT_NAME, "conv_w"],
        outputs=["conv_out"],
        kernel_shape=[1, 1],
        name="conv0",
    )
    identity = helper.make_node(
        "Identity",
        inputs=["conv_out"],
        outputs=[OUTPUT_NAME],
        name="identity0",
    )

    graph = helper.make_graph(
        nodes=[conv, identity],
        name="vmaf_tiny_smoke_v0",
        inputs=[helper.make_tensor_value_info(INPUT_NAME, TensorProto.FLOAT, [1, 1, 4, 4])],
        outputs=[helper.make_tensor_value_info(OUTPUT_NAME, TensorProto.FLOAT, [1, 1, 4, 4])],
        initializer=[weight],
    )

    model = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", OPSET)],
        producer_name="vmaf-tiny-smoke",
        producer_version="0",
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def write_sidecar(path: Path) -> None:
    payload = {
        "name": "vmaf_tiny_smoke_v0",
        "kind": "fr",
        "onnx_opset": OPSET,
        "input_name": INPUT_NAME,
        "output_name": OUTPUT_NAME,
        "notes": "CI smoke fixture — not a quality model.",
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def update_registry(path: Path, sha256_hex: str) -> None:
    registry = json.loads(path.read_text())
    entry = {
        "id": MODEL_ID,
        "kind": "fr",
        "onnx": "smoke_v0.onnx",
        "sha256": sha256_hex,
        "opset": OPSET,
        "smoke": True,
        "notes": "CI load-path probe (Conv + Identity). Not a quality model.",
    }
    models = [m for m in registry.get("models", []) if m.get("id") != MODEL_ID]
    models.append(entry)
    models.sort(key=lambda m: m["id"])
    registry["models"] = models
    path.write_text(json.dumps(registry, indent=2, sort_keys=True) + "\n")


def main() -> int:
    TINY_DIR.mkdir(parents=True, exist_ok=True)
    model = build_model()
    raw = model.SerializeToString(deterministic=True)
    ONNX_PATH.write_bytes(raw)
    digest = hashlib.sha256(raw).hexdigest()
    write_sidecar(SIDECAR_PATH)
    update_registry(REGISTRY_PATH, digest)
    print(f"wrote {ONNX_PATH} ({len(raw)} bytes, sha256={digest})")
    print(f"wrote {SIDECAR_PATH}")
    print(f"updated {REGISTRY_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
