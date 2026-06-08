#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Generate a PTQ calibration dataset from clip paths and frame counts.

Takes a set of clip paths and (optionally) a frame-count specification,
extracts representative frames, and writes a calibration JSONL / numpy
archive suitable for static post-training quantization via
``ptq_static.py``.

See docs/research/0006-tinyai-ptq-accuracy-targets.md for the calibration
strategy and dataset-size recommendations.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "gen_calibration.py: not yet implemented.\n"
    "See docs/research/0006-tinyai-ptq-accuracy-targets.md\n"
    "for the calibration strategy and dataset-size recommendations.\n"
    "Implement following the pattern in build_bisect_cache.py or ptq_static.py.",
    file=sys.stderr,
)
sys.exit(0)
