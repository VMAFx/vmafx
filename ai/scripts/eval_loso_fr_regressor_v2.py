#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Evaluate the codec-aware FR regressor v2 with leave-one-source-out cross-validation.

Runs LOSO CV across all corpus sources in the parquet feature table and
reports per-fold PLCC / SROCC, aggregated stats, and per-codec breakdown.

See docs/research/0067-fr-regressor-v2-prod-loso.md for the evaluation
protocol and expected results.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "eval_loso_fr_regressor_v2.py: not yet implemented.\n"
    "See docs/research/0067-fr-regressor-v2-prod-loso.md for the\n"
    "evaluation protocol and expected results.\n"
    "Implement following the pattern of eval_loso_vmaf_tiny_v3.py.",
    file=sys.stderr,
)
sys.exit(0)
