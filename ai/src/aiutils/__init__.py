# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared helper utilities for AI scripts.

This package centralizes common patterns (file hashing, time formatting,
CLI setup, Parquet I/O) to reduce code duplication across the ai/scripts/
directory and establish standard interfaces for new scripts.
"""

from aiutils.cli_helpers import add_batch_manifest_arguments, collect_cli_argv, make_argument_parser
from aiutils.file_utils import sha256, write_text_atomic
from aiutils.jsonl_utils import iter_jsonl
from aiutils.run_manifest import (
    build_run_manifest_payload,
    build_run_provenance,
    describe_path,
    write_manifest_json,
    write_run_manifest,
)
from aiutils.time_utils import now_iso_8601

# Optional heavy parquet helpers — only importable when pyarrow is installed.
# Imported eagerly here so that they appear in __all__ as defined names;
# the try/except ensures the rest of the package remains usable without pyarrow.
try:
    from aiutils.parquet_utils import (
        apply_standard_column_order,
        detect_schema_version,
        read_parquet_with_schema,
        write_parquet_atomic,
    )

    _PARQUET_AVAILABLE = True
except ImportError:
    _PARQUET_AVAILABLE = False

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
    "write_run_manifest",
    "write_text_atomic",
]

if _PARQUET_AVAILABLE:
    __all__ += [
        "apply_standard_column_order",
        "detect_schema_version",
        "read_parquet_with_schema",
        "write_parquet_atomic",
    ]


def __getattr__(name: str) -> object:
    """Lazy-load parquet helpers when pyarrow was not importable at init time."""
    _lazy = {
        "apply_standard_column_order",
        "detect_schema_version",
        "read_parquet_with_schema",
        "write_parquet_atomic",
    }
    if name in _lazy:
        import aiutils.parquet_utils as _pq

        return getattr(_pq, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
