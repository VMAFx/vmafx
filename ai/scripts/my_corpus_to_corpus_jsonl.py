#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Convert a custom MOS corpus to the shared CORPUS_ROW_KEYS JSONL format.

This is the template script for ingesting a new corpus not already covered
by the existing adapters (chug, konvid-1k, konvid-150k, lsvq, live-vqc,
waterloo-ivc, youtube-ugc, bvi-dvc).

See docs/ai/mos-corpora.md for the JSONL schema, required fields,
and the recommended implementation pattern.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "my_corpus_to_corpus_jsonl.py: this is a template stub.\n"
    "Copy it to a name matching your corpus (e.g. my_dataset_to_corpus_jsonl.py)\n"
    "and implement following the pattern in chug_to_corpus_jsonl.py or\n"
    "konvid_1k_to_corpus_jsonl.py.  See docs/ai/mos-corpora.md.",
    file=sys.stderr,
)
sys.exit(0)
