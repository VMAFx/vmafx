# Copyright 2026 Lusoris and Claude (Anthropic)
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared helper utilities for AI scripts.

This package centralizes common patterns (file hashing, time formatting,
CLI setup, Parquet I/O) to reduce code duplication across the ai/scripts/
directory and establish standard interfaces for new scripts.
"""

from aiutils.cli_helpers import add_batch_manifest_arguments, collect_cli_argv, make_argument_parser
from aiutils.file_utils import sha256
from aiutils.jsonl_utils import iter_jsonl
from aiutils.run_manifest import (
    build_run_manifest_payload,
    build_run_provenance,
    describe_path,
    write_manifest_json,
    write_run_manifest,
)
from aiutils.time_utils import now_iso_8601

__all__ = [
    "add_batch_manifest_arguments",
    "build_run_manifest_payload",
    "build_run_provenance",
    "collect_cli_argv",
    "describe_path",
    "iter_jsonl",
    "make_argument_parser",
    "now_iso_8601",
    "sha256",
    "write_manifest_json",
    "write_parquet_atomic",
    "write_run_manifest",
]


def __getattr__(name: str) -> object:
    """Import optional heavy helpers only when the caller asks for them."""
    if name == "write_parquet_atomic":
        from aiutils.parquet_utils import write_parquet_atomic

        return write_parquet_atomic
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
