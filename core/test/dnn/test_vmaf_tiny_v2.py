#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke test for the shipped ``vmaf_tiny_v2`` ONNX model.

Verifies, end-to-end, that:

  1. The registry entry for ``vmaf_tiny_v2`` exists, points to a
     real ONNX file, and the on-disk sha256 matches.
  2. The sidecar JSON is well-formed and pins the canonical input /
     output names + opset 17.
  3. The ONNX loads via onnxruntime, accepts ``[N, 6]`` float32
     input, and produces a finite ``[N]`` float32 output.
  4. The bundled scaler is wired correctly: feeding the per-feature
     training mean produces a finite output (equivalent to feeding
     a zero z-score vector to the MLP).

Skips cleanly when ``onnxruntime`` is not importable (the registry
sha256 + sidecar checks still execute).
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import unittest
from pathlib import Path

try:
    import numpy as np  # type: ignore[import-not-found]
    import onnxruntime as ort  # type: ignore[import-not-found]

    _ORT_IMPORT_ERR: str | None = None
except Exception as exc:
    np = None  # type: ignore[assignment]
    ort = None  # type: ignore[assignment]
    _ORT_IMPORT_ERR = str(exc)

REPO_ROOT = Path(__file__).resolve().parents[3]
TINY = REPO_ROOT / "model" / "tiny"
ONNX_PATH = TINY / "vmaf_tiny_v2.onnx"
SIDECAR_PATH = TINY / "vmaf_tiny_v2.json"
REGISTRY_PATH = TINY / "registry.json"

EXPECTED_FEATURES = [
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
]


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


class VmafTinyV2Smoke(unittest.TestCase):
    def setUp(self) -> None:
        if not ONNX_PATH.is_file():
            self.skipTest(f"{ONNX_PATH} missing — pre-shipping export not done yet")

    def test_registry_entry_present_and_digest_matches(self) -> None:
        reg = json.loads(REGISTRY_PATH.read_text())
        entry = next(
            (m for m in reg.get("models", []) if m.get("id") == "vmaf_tiny_v2"),
            None,
        )
        self.assertIsNotNone(entry, "registry.json missing vmaf_tiny_v2 entry")
        self.assertEqual(entry["onnx"], "vmaf_tiny_v2.onnx")
        self.assertEqual(entry["kind"], "fr")
        self.assertEqual(entry["opset"], 17)
        self.assertFalse(entry.get("smoke", False))
        self.assertEqual(entry["sha256"], _sha256(ONNX_PATH))

    def test_sidecar_well_formed(self) -> None:
        side = json.loads(SIDECAR_PATH.read_text())
        self.assertEqual(side["id"], "vmaf_tiny_v2")
        self.assertEqual(side["kind"], "fr")
        self.assertEqual(side["onnx"], "vmaf_tiny_v2.onnx")
        self.assertEqual(side["opset"], 17)
        self.assertEqual(side["input_name"], "features")
        self.assertEqual(side["output_name"], "vmaf")
        self.assertEqual(side["features"], EXPECTED_FEATURES)
        self.assertEqual(len(side["input_mean"]), 6)
        self.assertEqual(len(side["input_std"]), 6)

    def test_onnx_inference_shape_and_finite(self) -> None:
        if _ORT_IMPORT_ERR is not None:
            self.skipTest(f"onnxruntime not available: {_ORT_IMPORT_ERR}")

        sess = ort.InferenceSession(str(ONNX_PATH), providers=["CPUExecutionProvider"])
        ins = sess.get_inputs()
        outs = sess.get_outputs()
        self.assertEqual(len(ins), 1)
        self.assertEqual(len(outs), 1)
        self.assertEqual(ins[0].name, "features")
        self.assertEqual(outs[0].name, "vmaf")

        # 4 sample rows of canonical-6 features in plausible ranges.
        x = np.array(
            [
                [0.92, 0.51, 0.55, 0.50, 0.45, 6.3],
                [0.95, 0.62, 0.65, 0.60, 0.55, 1.5],
                [0.85, 0.32, 0.36, 0.31, 0.27, 12.1],
                [0.99, 0.85, 0.86, 0.84, 0.83, 0.5],
            ],
            dtype=np.float32,
        )
        (y,) = sess.run(None, {"features": x})
        self.assertEqual(y.shape, (4,))
        self.assertTrue(np.all(np.isfinite(y)))

    def test_bundled_scaler_zero_centred_at_mean(self) -> None:
        if _ORT_IMPORT_ERR is not None:
            self.skipTest(f"onnxruntime not available: {_ORT_IMPORT_ERR}")

        side = json.loads(SIDECAR_PATH.read_text())
        sess = ort.InferenceSession(str(ONNX_PATH), providers=["CPUExecutionProvider"])
        # Feeding the bundled mean -> z-score zero -> first hidden
        # layer activation is just the bias; output must be finite.
        mean = np.asarray(side["input_mean"], dtype=np.float32).reshape(1, -1)
        (y_mean,) = sess.run(None, {"features": mean})
        self.assertEqual(y_mean.shape, (1,))
        self.assertTrue(np.isfinite(y_mean[0]))


if __name__ == "__main__":
    # Allow running directly via `python3 test_vmaf_tiny_v2.py`.
    os.chdir(REPO_ROOT)
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
