# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared CLI helpers for AI operator scripts."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path


def collect_cli_argv(argv: Sequence[str] | None) -> list[str]:
    """Return the effective raw CLI argument vector for provenance."""
    source = sys.argv[1:] if argv is None else argv
    return [str(item) for item in source]


def make_argument_parser(
    *,
    description: str | None = None,
    prog: str | None = None,
) -> argparse.ArgumentParser:
    """Create the standard parser shape for AI scripts."""
    return argparse.ArgumentParser(
        prog=prog,
        description=description,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )


def add_batch_manifest_arguments(
    parser: argparse.ArgumentParser,
    *,
    allow_row_failures: bool = False,
) -> None:
    """Add the shared batch-manifest report arguments.

    Batch manifest runners differ in the table-specific fields they accept,
    but the operator-facing wrapper flags should stay identical across them.
    """
    parser.add_argument("--manifest", type=Path, required=True, help="Batch manifest JSON")
    parser.add_argument(
        "--base-dir",
        type=Path,
        default=None,
        help="Base directory for relative manifest paths; defaults to manifest directory",
    )
    parser.add_argument("--report-json", type=Path, default=None, help="Batch report JSON path")
    parser.add_argument("--report-md", type=Path, default=None, help="Batch report Markdown path")
    if allow_row_failures:
        parser.add_argument(
            "--allow-row-failures",
            action="store_true",
            help="Return success when tables completed but some rows failed",
        )
    parser.add_argument(
        "--fail-fast",
        action="store_true",
        help="Stop after the first failed table",
    )
