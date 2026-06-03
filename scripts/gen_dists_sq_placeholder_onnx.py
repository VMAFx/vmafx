#!/usr/bin/env python3
#
#  Copyright 2026 Lusoris
#  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
"""Generate the DISTS-Sq smoke placeholder ONNX fixture.

The real DISTS follow-up will port Ding et al.'s VGG/SqueezeNet weights.
This placeholder only locks the libvmaf extractor ABI:

  inputs:
    ref   float32[1, 3, H, W]  ImageNet-normalised RGB, NCHW
    dist  float32[1, 3, H, W]  ImageNet-normalised RGB, NCHW
  output:
    score float32[]            mean squared feature distance

It is deliberately marked ``smoke: true`` in the registry and documented as
non-production. The graph is tiny and deterministic so the registry digest is
stable across regeneration.
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
ONNX_PATH = TINY_DIR / "dists_sq.onnx"
SIDECAR_PATH = TINY_DIR / "dists_sq.json"
REGISTRY_PATH = TINY_DIR / "registry.json"

OPSET = 17
MODEL_ID = "dists_sq_placeholder_v0"


def build_model() -> onnx.ModelProto:
    """Build a two-input scalar smoke graph."""
    sub = helper.make_node("Sub", inputs=["ref", "dist"], outputs=["delta"], name="delta")
    sq = helper.make_node("Mul", inputs=["delta", "delta"], outputs=["delta_sq"], name="square")
    mean = helper.make_node(
        "ReduceMean",
        inputs=["delta_sq"],
        outputs=["score"],
        keepdims=0,
        name="mean_distance",
    )

    graph = helper.make_graph(
        nodes=[sub, sq, mean],
        name="vmaf_tiny_dists_sq_placeholder_v0",
        inputs=[
            helper.make_tensor_value_info("ref", TensorProto.FLOAT, [1, 3, "H", "W"]),
            helper.make_tensor_value_info("dist", TensorProto.FLOAT, [1, 3, "H", "W"]),
        ],
        outputs=[helper.make_tensor_value_info("score", TensorProto.FLOAT, [])],
        initializer=[],
    )

    model = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", OPSET)],
        producer_name="vmaf-tiny-dists-sq-placeholder",
        producer_version="0",
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def write_sidecar(path: Path) -> None:
    payload = {
        "input_name": "ref",
        "kind": "fr",
        "name": "vmaf_tiny_dists_sq_placeholder_v0",
        "notes": (
            "DISTS-Sq smoke placeholder. Two ImageNet-normalised RGB NCHW "
            "inputs named 'ref' and 'dist'; scalar 'score' output. The graph "
            "computes mean squared tensor distance only to exercise the "
            "libvmaf extractor and ORT load path. Real Ding et al. DISTS "
            "weights are tracked as T7-DISTS-followup."
        ),
        "onnx_opset": OPSET,
        "output_name": "score",
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def update_registry(path: Path, sha256_hex: str) -> None:
    registry = json.loads(path.read_text(encoding="utf-8"))
    entry = {
        "id": MODEL_ID,
        "kind": "fr",
        "license": "BSD-3-Clause-Plus-Patent",
        "license_url": "https://github.com/VMAFx/vmafx/blob/master/LICENSE",
        "notes": (
            "DISTS-Sq smoke placeholder. Synthetic mean-squared tensor distance "
            "over two ImageNet-normalised RGB NCHW inputs; not a production "
            "DISTS checkpoint. Real upstream-derived weights from Ding et al. "
            "are tracked as T7-DISTS-followup. See docs/ai/models/dists_sq.md "
            "and ADR-0236."
        ),
        "onnx": "dists_sq.onnx",
        "opset": OPSET,
        "sha256": sha256_hex,
        "sigstore_bundle": "dists_sq.onnx.sigstore.json",
        "smoke": True,
    }
    models = [model for model in registry.get("models", []) if model.get("id") != MODEL_ID]
    models.insert(0, entry)
    registry["models"] = models
    path.write_text(json.dumps(registry, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    TINY_DIR.mkdir(parents=True, exist_ok=True)
    raw = build_model().SerializeToString(deterministic=True)
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
