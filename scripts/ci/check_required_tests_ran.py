#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify that required tests ran and were neither skipped nor absent in JUnit XML."""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Sequence


class CheckRequiredTestsError(Exception):
    """Raised when required tests were absent, skipped, or failed."""


def _matches(case: ET.Element, pattern: str) -> bool:
    """Check if a testcase matches a pattern against file, classname, or name."""
    classname = case.get("classname", "")
    name = case.get("name", "")
    full_name = f"{classname}::{name}"
    file_attr = case.get("file", "")
    stem = Path(pattern).stem

    if pattern in full_name or pattern in file_attr:
        return True
    if stem and stem in classname:
        return True
    return False


def verify_junit_tests(
    junit_path: str | Path,
    required: Sequence[str],
) -> list[str]:
    """Parse JUnit XML and verify all required tests ran, returning verified test names."""
    path = Path(junit_path)
    if not path.is_file():
        raise CheckRequiredTestsError(f"JUnit XML file not found: {path}")

    try:
        tree = ET.parse(path)  # noqa: S314
    except (ET.ParseError, OSError) as exc:
        raise CheckRequiredTestsError(f"failed to parse JUnit XML {path}: {exc}") from exc

    cases = list(tree.iter("testcase"))
    if not cases:
        raise CheckRequiredTestsError(f"zero tests refused: {path} contains no testcase records")

    verified: list[str] = []
    for req in required:
        matching = [c for c in cases if _matches(c, req)]
        if not matching:
            raise CheckRequiredTestsError(
                f"absent row refused: no testcase matched required '{req}'"
            )

        for case in matching:
            case_id = f"{case.get('classname', '')}::{case.get('name', '')}"
            skip = case.find("skipped")
            if skip is not None:
                msg = skip.get("message") or (skip.text or "").strip()
                raise CheckRequiredTestsError(
                    f"skipped row refused: test '{case_id}' was skipped: {msg}"
                )
            if case.find("failure") is not None or case.find("error") is not None:
                raise CheckRequiredTestsError(
                    f"test failure refused: test '{case_id}' did not pass"
                )
            verified.append(case_id)

    return verified


def main(argv: Sequence[str] | None = None) -> int:
    """CLI entry point for required tests checker."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("positional", nargs="*", help="Optional [junit_path] [required...]")
    parser.add_argument("--junit", dest="junit", help="Path to pytest JUnit XML file")
    parser.add_argument(
        "--required",
        dest="required",
        action="append",
        default=[],
        help="Required test or pattern (can be repeated)",
    )
    parser.add_argument(
        "--required-file",
        dest="required_files",
        action="append",
        default=[],
        help="Required test file (can be repeated)",
    )
    args = parser.parse_args(argv)

    junit_path = args.junit
    required = list(args.required) + list(args.required_files)

    if not junit_path and args.positional:
        junit_path = args.positional[0]
        required.extend(args.positional[1:])
    elif args.positional:
        required.extend(args.positional)

    if not junit_path:
        sys.stderr.write("error: JUnit XML path is required (use --junit or positional argument)\n")
        return 2

    if not required:
        sys.stderr.write("error: at least one required test/file must be specified\n")
        return 2

    try:
        verified = verify_junit_tests(junit_path, required)
        sys.stdout.write(f"ok: verified {len(verified)} required test executions in {junit_path}\n")
        return 0
    except CheckRequiredTestsError as exc:
        sys.stderr.write(f"error: {exc}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
