#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Parse and validate official CodeQL CSV results for cpp/unused-static-function.

The official CodeQL CLI emits a 9-column CSV when run with:
  codeql database analyze --format=csv --output=<file> <db> <query>

Schema:
  0: name (query name, str)
  1: description (query description, str)
  2: severity (expected recommendation, str)
  3: message (diagnostic message, str)
  4: path (source path relative to repository root, str)
  5: start_line (1-indexed start line, int)
  6: start_column (1-indexed start column, int)
  7: end_line (1-indexed end line, int)
  8: end_column (1-indexed end column, int)

Fail-closed contract:
  - Any non-empty row with != 9 columns raises SchemaError.
  - Query name, description, severity, and message shape must match the pinned query.
  - Any non-integer or <= 0 line/column field raises SchemaError.
  - Any empty path raises SchemaError.
  - Any alert matching selected lane target paths triggers failure (exit 1).
"""

from __future__ import annotations

import csv
import sys
from collections.abc import Iterable
from pathlib import Path
from typing import TextIO

DEFAULT_LANE_TARGET_PATHS: frozenset[str] = frozenset(
    {
        "core/src/pdjson.c",
        "core/src/picture.c",
        "core/src/thread_pool.c",
        "core/test/test_fex_ctx_vector.cpp",
        "core/test/test_thread_pool_backpressure.c",
    }
)

CODEQL_CSV_COLUMN_COUNT: int = 9
EXPECTED_QUERY_NAME = "Unused static function"
EXPECTED_QUERY_DESCRIPTION = (
    "A static function that is never called or accessed may be an indication that "
    "the code is incomplete or has a typo."
)
EXPECTED_SEVERITY = "recommendation"


class CodeQLSchemaError(ValueError):
    """Raised when a CodeQL CSV row violates the official 9-column schema."""


def parse_codeql_csv(stream_or_path: str | Path | TextIO) -> list[dict[str, object]]:
    """Parse official CodeQL CSV output and enforce the 9-column schema fail-closed."""
    if isinstance(stream_or_path, (str, Path)):
        with Path(stream_or_path).open(encoding="utf-8", newline="") as stream:
            return _parse_rows(csv.reader(stream))
    return _parse_rows(csv.reader(stream_or_path))


def _validate_query_fields(line_idx: int, row: list[str]) -> tuple[str, str, str, str]:
    name, description, severity, message = row[:4]
    if name != EXPECTED_QUERY_NAME:
        raise CodeQLSchemaError(
            f"Line {line_idx}: unexpected query name {name!r}; " f"expected {EXPECTED_QUERY_NAME!r}"
        )
    if description != EXPECTED_QUERY_DESCRIPTION:
        raise CodeQLSchemaError(f"Line {line_idx}: unexpected query description {description!r}")
    if severity != EXPECTED_SEVERITY:
        raise CodeQLSchemaError(
            f"Line {line_idx}: unexpected severity {severity!r}; " f"expected {EXPECTED_SEVERITY!r}"
        )
    if not message.startswith("Static function ") or " is unreachable" not in message:
        raise CodeQLSchemaError(f"Line {line_idx}: unexpected unused-static message {message!r}")
    return name, description, severity, message


def _parse_location(line_idx: int, row: list[str]) -> tuple[int, int, int, int]:
    try:
        location = tuple(int(value) for value in row[5:9])
    except ValueError as err:
        raise CodeQLSchemaError(
            f"Line {line_idx}: non-integer location coordinates: {err}"
        ) from err
    start_line, start_column, end_line, end_column = location
    invalid = (
        min(location) <= 0
        or end_line < start_line
        or (end_line == start_line and end_column < start_column)
    )
    if invalid:
        raise CodeQLSchemaError(
            f"Line {line_idx}: invalid location coordinates "
            f"({start_line}:{start_column}-{end_line}:{end_column})"
        )
    return start_line, start_column, end_line, end_column


def _parse_rows(reader: Iterable[list[str]]) -> list[dict[str, object]]:
    parsed: list[dict[str, object]] = []
    for line_idx, row in enumerate(reader, start=1):
        if not row:
            continue
        if len(row) != CODEQL_CSV_COLUMN_COUNT:
            raise CodeQLSchemaError(
                f"Line {line_idx}: invalid CodeQL CSV schema: expected "
                f"{CODEQL_CSV_COLUMN_COUNT} columns, got {len(row)}: {row!r}"
            )
        name, description, severity, message = _validate_query_fields(line_idx, row)
        path = row[4].replace("\\", "/").lstrip("/")
        if not path:
            raise CodeQLSchemaError(f"Line {line_idx}: empty file path in row: {row!r}")
        start_line, start_column, end_line, end_column = _parse_location(line_idx, row)
        parsed.append(
            {
                "name": name,
                "description": description,
                "severity": severity,
                "message": message,
                "path": path,
                "start_line": start_line,
                "start_column": start_column,
                "end_line": end_line,
                "end_column": end_column,
            }
        )
    return parsed


def evaluate_unused_static_results(
    rows: list[dict[str, object]],
    target_paths: frozenset[str] = DEFAULT_LANE_TARGET_PATHS,
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    """Split results into lane target violations and untargeted repo-wide inventory."""
    violations: list[dict[str, object]] = []
    untargeted: list[dict[str, object]] = []
    for r in rows:
        path = str(r["path"])
        if path in target_paths:
            violations.append(r)
        else:
            untargeted.append(r)
    return violations, untargeted


def main(argv: list[str] | None = None) -> int:
    args = argv if argv is not None else sys.argv[1:]
    if len(args) != 1:
        print(f"Usage: {sys.argv[0]} <codeql-unused-static.csv>", file=sys.stderr)
        return 2

    csv_path = Path(args[0])
    if not csv_path.is_file():
        print(f"Error: file not found: {csv_path}", file=sys.stderr)
        return 2

    try:
        rows = parse_codeql_csv(csv_path)
    except CodeQLSchemaError as err:
        print(f"FAIL (schema error): {err}", file=sys.stderr)
        return 2

    violations, untargeted = evaluate_unused_static_results(rows)
    if violations:
        print(
            f"FAIL: {len(violations)} selected unused-static finding(s) remain in lane targets:",
            file=sys.stderr,
        )
        for v in violations:
            print(
                f"  {v['path']}:{v['start_line']}:{v['start_column']}: {v['message']}",
                file=sys.stderr,
            )
        return 1

    print(f"PASS: 0 selected rows in lane targets ({len(rows)} repository-wide row(s) inventoried)")
    for u in untargeted:
        print(f"  inventory: {u['path']}:{u['start_line']}: {u['message']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
