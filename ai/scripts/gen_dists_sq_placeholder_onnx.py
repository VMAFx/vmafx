#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Generate a tiny placeholder ONNX for the DISTS-SQ extractor.

Produces a smoke-only ONNX that exercises the DISTS-SQ C extractor load /
session plumbing without requiring the full DISTS training pipeline.

See docs/ai/models/dists_sq.md and docs/research/0111-dists-sq-extractor-2026-05-14.md.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "gen_dists_sq_placeholder_onnx.py: not yet implemented.\n"
    "See docs/ai/models/dists_sq.md for the intended usage and\n"
    "docs/research/0111-dists-sq-extractor-2026-05-14.md for context.\n"
    "Implement following the pattern in export_fastdvdnet_pre_placeholder.py.",
    file=sys.stderr,
)
sys.exit(1)
