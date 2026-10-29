#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check that GitHub Actions workflow files stay within the verification input size bound.

Bound matches Praetor engine internal/adopt/verification_inputs.go maxVerificationInputBytes = 64*1024.
Oversize files cause metadata byte bound errors during engine audit checks.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

DEFAULT_MAX_BYTES = 65536  # 64 * 1024


def check_workflow_sizes(workflow_paths: list[Path], max_bytes: int) -> list[tuple[Path, int]]:
    """Return a list of (path, size) tuples for workflows that exceed max_bytes."""
    oversized: list[tuple[Path, int]] = []
    for path in workflow_paths:
        size = path.stat().st_size
        if size > max_bytes:
            oversized.append((path, size))
    return oversized


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify .github/workflows/*.yml and *.yaml do not exceed maximum byte bounds."
    )
    parser.add_argument(
        "--max-bytes",
        type=int,
        default=DEFAULT_MAX_BYTES,
        help=f"Maximum allowed file size in bytes (default: {DEFAULT_MAX_BYTES})",
    )
    parser.add_argument(
        "files",
        nargs="*",
        type=Path,
        help="Optional workflow files to check. If empty, scans .github/workflows/*.yml and *.yaml",
    )
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[2]
    if args.files:
        paths = [p.resolve() for p in args.files]
    else:
        workflows_dir = root / ".github" / "workflows"
        if not workflows_dir.is_dir():
            print(f"error: {workflows_dir} is not a directory", file=sys.stderr)
            return 1
        paths = sorted(
            p for p in workflows_dir.iterdir() if p.is_file() and p.suffix in (".yml", ".yaml")
        )

    oversized = check_workflow_sizes(paths, args.max_bytes)
    if oversized:
        for path, size in oversized:
            rel = path.relative_to(root) if path.is_relative_to(root) else path
            print(
                f"error: {rel} ({size} bytes) exceeds {args.max_bytes} byte limit",
                file=sys.stderr,
            )
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
