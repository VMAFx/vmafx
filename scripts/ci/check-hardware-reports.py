#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Validate the tester reports under docs/hardware-reports/.

A report is the JSON that `vmaf-tester-report` (the tester image) prints. This
gate keeps a hand-edited or hand-made file out of the tree: file name, JSON
schema, the integrity hash the tool computes, and the facts only an image built by
the hosted workflow can have. A failing report (verdict `fail`) is accepted: it is
the most useful kind. `incomplete` (a maintainer-only option) is refused.

Usage: check-hardware-reports.py [--dir DIR | --report FILE]   (default docs/hardware-reports)
Exit: 0 clean (also when there are no reports); 1 a violation; 2 jsonschema missing.
"""

from __future__ import annotations

import argparse
import importlib
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "rc1-tester" / "src"))

from vmaf_rc1_tester.hw_facts import ALLOWED_CPUINFO_KEYS  # noqa: E402
from vmaf_rc1_tester.hw_report import report_digest  # noqa: E402

SCHEMA_NAME = "report.schema.json"
NAME_RE = re.compile(r"^(\d{4}-\d{2}-\d{2})-[a-z0-9][a-z0-9-]{1,60}\.json$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")


def policy_errors(name: str | None, report: dict[str, Any]) -> list[str]:
    """Violations of the rules the schema cannot express."""
    errors = []
    match = NAME_RE.match(name) if name is not None else None
    if name is not None and not match:
        errors.append("file name must be YYYY-MM-DD-<cpu-slug>.json (lower case, digits, '-')")
    elif match and not report["generated_utc"].startswith(match.group(1)):
        errors.append("the date in the file name is not the report's generated_utc date")
    if report_digest(report) != report["report_sha256"]:
        errors.append("report_sha256 does not match the content (edited by hand?)")
    image = report["image"]
    if not image["built_by_workflow"] or not COMMIT_RE.match(image["source_commit"]):
        errors.append("the image was not built by the hosted workflow from a commit")
    if not image["files_match_build"]:
        errors.append("image.files_match_build is false: the binaries differ from the build")
    unknown = set(report["host"]["cpuinfo"]) - ALLOWED_CPUINFO_KEYS
    if unknown:
        errors.append(f"host.cpuinfo holds keys outside the allow-list: {sorted(unknown)}")
    errors += verdict_errors(report)
    return errors


def verdict_errors(report: dict[str, Any]) -> list[str]:
    """The verdict must follow from the sections."""
    verdict = report["verdict"]
    if verdict == "incomplete":
        return ["verdict 'incomplete' (a check was skipped) is not accepted"]
    all_good = (
        report["dispatch_equivalence"]["status"] == "identical"
        and report["reference_equivalence"]["status"] == "identical"
        and report["metal_equivalence"]["status"] in ("identical", "no_device", "not_applicable")
        # ADR-1496: schema 2 reports carry the parity gate's Metal cells.
        and report.get("metal_gate", {"status": "not_applicable"})["status"]
        in ("pass", "no_device", "not_applicable")
        and report["unit_tests"]["status"] == "pass"
        and report["golden_gate"]["status"] in ("pass", "not_applicable")
        and report["image"]["files_match_build"]
    )
    if verdict == "pass" and not all_good:
        return ["verdict 'pass' but a section does not pass"]
    if verdict == "fail" and (all_good or not report["failed_checks"]):
        return ["verdict 'fail' without a failing section or failed_checks"]
    return []


def check(directory: Path, single: Path | None = None, *, check_name: bool = True) -> int:
    paths = [single] if single else [
        p for p in sorted(directory.glob("*.json")) if p.name != SCHEMA_NAME
    ]  # fmt: skip
    if not paths:
        return 0
    try:
        jsonschema = importlib.import_module("jsonschema")  # needed only when reports exist
    except ImportError:
        print("error: python package jsonschema is required", file=sys.stderr)
        return 2
    schema = json.loads((directory / SCHEMA_NAME).read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    failed = 0
    for path in paths:
        try:
            report = json.loads(path.read_text(encoding="utf-8"))
        except ValueError as error:
            problems = [f"not JSON: {error}"]
        else:
            problems = [f"schema: {e.message[:160]}" for e in validator.iter_errors(report)]
            problems = problems or policy_errors(path.name if check_name else None, report)
        for problem in problems:
            print(f"::error file={path}::{problem}", file=sys.stderr)
        failed += bool(problems)
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dir", default=str(ROOT / "docs" / "hardware-reports"))
    parser.add_argument(
        "--report",
        help="check one report file wherever it is (CI of the bundle); no file-name rule",
    )
    args = parser.parse_args()
    single = Path(args.report) if args.report else None
    return check(Path(args.dir), single, check_name=single is None)


if __name__ == "__main__":
    sys.exit(main())
