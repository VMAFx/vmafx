#!/usr/bin/env python3
#
#  Copyright 2026 Lusoris
#  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
"""Generate the MobileSal saliency placeholder ONNX fixture (T6-2a).

This is a *smoke-only* synthetic placeholder that matches the I/O contract
of MobileSal — "MobileSal: Extremely Efficient RGB-D Salient Object
Detection" (Wu, Liu, Cheng et al.), the model targeted by the Wave 1
roadmap §2.3 — *without* shipping the real upstream weights. The contract:

  inputs:
    input  float32[1, 3, H, W]   ImageNet-normalised RGB, NCHW
  outputs:
    saliency_map  float32[1, 1, H, W]   per-pixel saliency in [0, 1]

The graph is a single 3-to-1 1x1 Conv with constant weights followed by a
Sigmoid; output is therefore ~0.5 everywhere on a uniform input. That
is intentional — the placeholder exists to (a) exercise the load path and
shape-check logic in ``feature_mobilesal.c`` end-to-end, (b) lock down
the registry digest, and (c) keep the per-PR doc surface aligned with
``docs/ai/models/mobilesal.md`` while real upstream weights are tracked
as the T6-2a-followup task.

Re-running this script with the same numpy/onnx versions must produce
byte-identical output (deterministic IR, no doc_string, fixed
producer_version) — the registry digest is asserted by the runtime
loader before CreateSession.

Usage:  .venv/bin/python scripts/gen_mobilesal_placeholder_onnx.py
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
ONNX_PATH = TINY_DIR / "mobilesal.onnx"
SIDECAR_PATH = TINY_DIR / "mobilesal.json"
REGISTRY_PATH = TINY_DIR / "registry.json"

OPSET = 17
MODEL_ID = "mobilesal_placeholder_v0"
INPUT_NAME = "input"
OUTPUT_NAME = "saliency_map"


def build_model() -> onnx.ModelProto:
    """Build the placeholder graph.

    Shape: input [1, 3, H, W] float32 → Conv 3→1 1x1 → Sigmoid → [1, 1, H, W].
    H, W are dynamic so the C side can match the source resolution.
    Weights average the three input channels with a small bias to keep
    the saliency mass near 0.5 on ImageNet-normalised inputs.
    """
    weights = np.full((1, 3, 1, 1), 1.0 / 3.0, dtype=np.float32)
    bias = np.zeros((1,), dtype=np.float32)

    conv_w = numpy_helper.from_array(weights, name="conv_w")
    conv_b = numpy_helper.from_array(bias, name="conv_b")

    conv = helper.make_node(
        "Conv",
        inputs=[INPUT_NAME, "conv_w", "conv_b"],
        outputs=["conv_out"],
        kernel_shape=[1, 1],
        name="conv0",
    )
    sigmoid = helper.make_node(
        "Sigmoid",
        inputs=["conv_out"],
        outputs=[OUTPUT_NAME],
        name="sigmoid0",
    )

    graph = helper.make_graph(
        nodes=[conv, sigmoid],
        name="vmaf_tiny_mobilesal_placeholder_v0",
        inputs=[helper.make_tensor_value_info(INPUT_NAME, TensorProto.FLOAT, [1, 3, "H", "W"])],
        outputs=[helper.make_tensor_value_info(OUTPUT_NAME, TensorProto.FLOAT, [1, 1, "H", "W"])],
        initializer=[conv_w, conv_b],
    )

    model = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", OPSET)],
        producer_name="vmaf-tiny-mobilesal-placeholder",
        producer_version="0",
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def write_sidecar(path: Path) -> None:
    payload = {
        "input_name": INPUT_NAME,
        "kind": "nr",
        "name": "vmaf_tiny_mobilesal_placeholder_v0",
        "notes": (
            "MobileSal saliency placeholder (T6-2a smoke-only). Synthetic 3->1 "
            "Conv+Sigmoid that matches the MobileSal I/O contract: ImageNet-"
            "normalised RGB NCHW [1,3,H,W] -> saliency map NCHW [1,1,H,W] in "
            "[0,1]. Real-weights swap deferred per ADR-0257 (MobileSal "
            "CC BY-NC-SA 4.0) + ADR-0265 (U-2-Net u2netp distribution + "
            "Resize op blockers). See docs/ai/models/mobilesal.md."
        ),
        "onnx_opset": OPSET,
        "output_name": OUTPUT_NAME,
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def update_registry(path: Path, sha256_hex: str) -> None:
    registry = json.loads(path.read_text())
    entry = {
        "id": MODEL_ID,
        "kind": "nr",
        "notes": (
            "MobileSal saliency placeholder (T6-2a smoke-only). Synthetic "
            "Conv+Sigmoid matching the upstream I/O contract; per-pixel "
            "saliency map in [0,1]. Real-weights swap deferred: ADR-0257 "
            "(upstream MobileSal CC BY-NC-SA 4.0 + Drive-walled + RGB-D) "
            "and ADR-0265 (U-2-Net u2netp replacement: Apache-2.0 OK but "
            "Drive-walled + Resize op not on allowlist). See ADR-0218 / "
            "ADR-0257 / ADR-0265 + docs/ai/models/mobilesal.md."
        ),
        "onnx": "mobilesal.onnx",
        "opset": OPSET,
        "sha256": sha256_hex,
        "smoke": True,
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
