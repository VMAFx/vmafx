#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Convert an HDR/SDR VQA dataset to the shared CORPUS_ROW_KEYS JSONL format.

Adapter for ingesting HDR/SDR paired VQA corpora — used by the
panel-aware recommendation pipeline (ADR-0459) to incorporate HDR/SDR
subjective scores into training.

See docs/adr/0459-vmaftune-panel-aware-recommendations.md for context.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "hdrsdr_vqa_to_corpus_jsonl.py: not yet implemented.\n"
    "See docs/adr/0459-vmaftune-panel-aware-recommendations.md\n"
    "for context and the intended corpus format.\n"
    "Implement following the pattern in chug_to_corpus_jsonl.py.",
    file=sys.stderr,
)
sys.exit(0)
