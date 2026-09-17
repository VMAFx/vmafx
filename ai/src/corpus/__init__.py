# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared corpus-ingestion infrastructure (ADR-0371)."""

from .base import (
    CorpusIngestBase,
    RunStats,
    load_progress,
    mark_done,
    mark_failed,
    probe_geometry,
    save_progress,
    sha256_file,
    should_attempt,
    utc_now_iso,
)

__all__ = [
    "CorpusIngestBase",
    "RunStats",
    "load_progress",
    "mark_done",
    "mark_failed",
    "probe_geometry",
    "save_progress",
    "sha256_file",
    "should_attempt",
    "utc_now_iso",
]
