#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Fetch the LSVQ (Large Scale Video Quality) dataset for NR training.

Downloads LSVQ clips and MOS labels, validates checksums, and writes
a corpus root compatible with ``lsvq_to_corpus_jsonl.py``.

See docs/research/0086-tiny-ai-sota-deep-dive-2026-05-08.md (LSVQ section)
for corpus context and corpus-ingestion ADR.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "fetch_lsvq.py: not yet implemented.\n"
    "See docs/research/0086-tiny-ai-sota-deep-dive-2026-05-08.md\n"
    "for corpus context.  Implement following the pattern in\n"
    "fetch_konvid_1k.py and pair with lsvq_to_corpus_jsonl.py.",
    file=sys.stderr,
)
sys.exit(0)
