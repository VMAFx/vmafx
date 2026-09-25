#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Ratchet research-digest identifiers and their H1 headings.

The research tree predates its one-ID-per-digest rule and still contains
legacy collisions and non-canonical headings.  The generated baseline records
those exact sets without making them valid.  New drift fails closed; an
intentional cleanup must regenerate the baseline with ``--write``.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

DIGEST_NAME = re.compile(r"^(?P<number>[0-9]{4})-.+\.md$")
RESEARCH_H1 = re.compile(r"^# Research-(?P<number>[0-9]{4})(?=$|[:\s\u2013\u2014-])")
SCHEMA_VERSION = 1
MIN_COLLISION_MEMBERS = 2
DEFAULT_BASELINE = Path("scripts/ci/research-digest-id-baseline.json")
TEMPLATE_NAME = "0000-template.md"


class GateError(RuntimeError):
    """A deterministic research-digest contract failure."""


def _first_h1(path: Path) -> str:
    """Return the first level-one Markdown heading, or an empty string."""
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise GateError(f"cannot read {path}: {exc}") from exc
    return next((line for line in text.splitlines() if line.startswith("# ")), "")


def scan_repository(root: Path) -> tuple[dict[str, list[str]], dict[str, str]]:
    """Return current collision sets and non-canonical H1 headings."""
    research_dir = root / "docs/research"
    if not research_dir.is_dir():
        raise GateError(f"research directory does not exist: {research_dir}")

    by_number: dict[str, list[str]] = defaultdict(list)
    heading_exceptions: dict[str, str] = {}
    for path in sorted(research_dir.glob("*.md")):
        match = DIGEST_NAME.fullmatch(path.name)
        if match is None or path.name == TEMPLATE_NAME:
            continue

        number = match.group("number")
        relative = path.relative_to(root).as_posix()
        by_number[number].append(relative)

        h1 = _first_h1(path)
        h1_match = RESEARCH_H1.match(h1)
        if h1_match is None or h1_match.group("number") != number:
            heading_exceptions[relative] = h1

    collisions = {
        number: sorted(paths) for number, paths in sorted(by_number.items()) if len(paths) > 1
    }
    return collisions, dict(sorted(heading_exceptions.items()))


def _baseline_payload(root: Path) -> dict[str, Any]:
    collisions, heading_exceptions = scan_repository(root)
    return {
        "schema_version": SCHEMA_VERSION,
        "legacy_collisions": collisions,
        "legacy_heading_exceptions": heading_exceptions,
    }


def _new_debt_errors(previous: dict[str, Any], current: dict[str, Any]) -> list[str]:
    """Reject baseline writes that would absorb new or renamed debt."""
    errors: list[str] = []
    previous_collisions = previous["legacy_collisions"]
    for number, paths in current["legacy_collisions"].items():
        old_paths = previous_collisions.get(number, [])
        added = sorted(set(paths) - set(old_paths))
        if added:
            errors.append(f"Research-{number} adds collision members {added!r}")

    previous_headings = previous["legacy_heading_exceptions"]
    for relative, heading in current["legacy_heading_exceptions"].items():
        if previous_headings.get(relative) != heading:
            errors.append(f"{relative} adds or changes a non-canonical H1")
    return errors


def write_baseline(root: Path, baseline_path: Path) -> None:
    """Write only equal or reduced legacy debt as stable, sorted JSON."""
    payload = _baseline_payload(root)
    if baseline_path.exists():
        growth = _new_debt_errors(load_baseline(baseline_path), payload)
        if growth:
            details = "; ".join(growth)
            raise GateError(f"refusing to absorb new research-digest debt: {details}")
    baseline_path.parent.mkdir(parents=True, exist_ok=True)
    baseline_path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def load_baseline(path: Path) -> dict[str, Any]:
    """Load and validate the generated baseline schema."""
    try:
        raw = path.read_text(encoding="utf-8")
        payload = json.loads(raw)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise GateError(f"cannot load baseline {path}: {exc}") from exc

    expected_keys = {
        "schema_version",
        "legacy_collisions",
        "legacy_heading_exceptions",
    }
    if not isinstance(payload, dict) or set(payload) != expected_keys:
        raise GateError(f"baseline {path} has an invalid top-level schema")
    if payload["schema_version"] != SCHEMA_VERSION:
        raise GateError(
            f"baseline {path} schema_version must be {SCHEMA_VERSION}, "
            f"got {payload['schema_version']!r}"
        )

    collisions = payload["legacy_collisions"]
    headings = payload["legacy_heading_exceptions"]
    if not isinstance(collisions, dict) or not isinstance(headings, dict):
        raise GateError(f"baseline {path} collision and heading maps must be objects")

    for number, paths in collisions.items():
        if (
            not isinstance(number, str)
            or re.fullmatch(r"[0-9]{4}", number) is None
            or not isinstance(paths, list)
            or len(paths) < MIN_COLLISION_MEMBERS
            or paths != sorted(set(paths))
            or not all(isinstance(item, str) for item in paths)
        ):
            raise GateError(f"baseline {path} has an invalid collision entry for {number!r}")
    if not all(isinstance(key, str) and isinstance(value, str) for key, value in headings.items()):
        raise GateError(f"baseline {path} has an invalid heading exception entry")

    canonical = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if raw != canonical:
        raise GateError(f"baseline {path} is not in deterministic generated form")
    return payload


def audit_repository(root: Path, baseline_path: Path) -> list[str]:
    """Compare the current tree with its exact legacy-debt baseline."""
    baseline = load_baseline(baseline_path)
    collisions, heading_exceptions = scan_repository(root)
    errors: list[str] = []

    expected_collisions = baseline["legacy_collisions"]
    for number in sorted(set(expected_collisions) | set(collisions)):
        expected = expected_collisions.get(number, [])
        actual = collisions.get(number, [])
        if actual != expected:
            errors.append(
                f"Research-{number} collision set drift: expected {expected!r}, "
                f"found {actual!r}"
            )

    expected_headings = baseline["legacy_heading_exceptions"]
    for relative in sorted(set(expected_headings) | set(heading_exceptions)):
        expected = expected_headings.get(relative)
        actual = heading_exceptions.get(relative)
        if actual != expected:
            errors.append(
                f"{relative} H1 drift: expected legacy exception {expected!r}, "
                f"found {actual!r}; canonical form is '# Research-NNNN: ...'"
            )
    return errors


def _resolve_baseline(root: Path, value: Path | None) -> Path:
    if value is None:
        return root / DEFAULT_BASELINE
    return value if value.is_absolute() else root / value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (default: inferred from this script)",
    )
    parser.add_argument(
        "--baseline",
        type=Path,
        help=f"baseline path (default: {DEFAULT_BASELINE})",
    )
    parser.add_argument(
        "--write",
        action="store_true",
        help="regenerate the exact legacy-debt baseline",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()
    baseline_path = _resolve_baseline(root, args.baseline)

    try:
        if args.write:
            write_baseline(root, baseline_path)
            collisions, headings = scan_repository(root)
            print(
                "research-digest ID baseline written: "
                f"{len(collisions)} collision sets, {len(headings)} H1 exceptions"
            )
            return 0

        errors = audit_repository(root, baseline_path)
    except GateError as exc:
        print(f"research-digest ID gate: {exc}", file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(f"research-digest ID gate: {error}", file=sys.stderr)
        print(
            "research-digest ID gate failed; fix the drift or explicitly regenerate "
            "the reviewed baseline with --write",
            file=sys.stderr,
        )
        return 1

    print("research-digest ID gate: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
