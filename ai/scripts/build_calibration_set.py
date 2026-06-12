#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Build a PTQ calibration set from the training corpus.

Selects a representative subset of frames from the training corpus
(stratified by source, codec, and quality tier) and writes the
calibration dataset used by ``ptq_static.py``.

See docs/ai/quantization.md for the calibration workflow and
dataset-size requirements.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "build_calibration_set.py: not yet implemented.\n"
    "See docs/ai/quantization.md for the calibration workflow.\n"
    "Implement following the pattern in extract_k150k_features.py\n"
    "for the corpus-iteration side and ptq_static.py for the quantization side.",
    file=sys.stderr,
)
sys.exit(1)
