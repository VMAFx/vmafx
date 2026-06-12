#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Quantize a trained ONNX model to INT8 using static PTQ.

Produces a quantized ``model/*_int8.onnx`` alongside the float32 original.
The quantization uses a calibration dataset built by ``build_calibration_set.py``
or ``gen_calibration.py``.

See docs/research/0090-phase-a-promotion-audit-2026-05-08.md for the
quantization strategy and accuracy-drop targets.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "quantize_int8.py: not yet implemented.\n"
    "See docs/research/0090-phase-a-promotion-audit-2026-05-08.md\n"
    "for the quantization strategy and accuracy-drop targets.\n"
    "Implement using onnxruntime.quantization.quantize_static following\n"
    "the pattern in ptq_static.py.",
    file=sys.stderr,
)
sys.exit(1)
