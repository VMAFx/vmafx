#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hold the number of pull-request workflows under the Scorecard window.

OpenSSF Scorecard's SAST check (scorecard v5.6.0) reads the check suites of a
commit with the GraphQL field ``checkSuites(first: 30)``. Every workflow a
``pull_request`` event starts is one check suite, and the GitHub Apps that
report on a pull request add more. A workflow over the window can push CodeQL
out of it; the commit then counts as unscanned and the SAST score falls.

This check counts the workflows under ``.github/workflows/`` whose ``on:``
names ``pull_request`` and fails when the count passes ``BUDGET``. The budget
leaves room for the CodeQL suite and the app suites inside the 30. A new
pull-request lane goes into an existing workflow as a job, or into
``pr-label-lanes.yml`` as a reusable-workflow call when a label starts it.

Exit 0 at or below the budget, 1 above it, 2 on a workflow it cannot read.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = ROOT / ".github" / "workflows"
BUDGET = 27


def triggers(doc: Any) -> list[str]:
    """The event names of a parsed workflow's ``on:``, in any of its three forms."""
    if not isinstance(doc, dict):
        return []
    # PyYAML reads the bare key `on` as the boolean True (YAML 1.1).
    on = doc.get("on", doc.get(True))
    if isinstance(on, str):
        return [on]
    if isinstance(on, list):
        return [str(event) for event in on]
    if isinstance(on, dict):
        return [str(event) for event in on]
    return []


def pr_workflows(directory: Path) -> list[str]:
    """The file names of the workflows a ``pull_request`` event starts."""
    names: list[str] = []
    for path in sorted(directory.iterdir()):
        if path.suffix not in (".yml", ".yaml") or not path.is_file():
            continue
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        if "pull_request" in triggers(doc):
            names.append(path.name)
    return names


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dir", type=Path, default=WORKFLOW_DIR, help="workflow directory")
    parser.add_argument("--budget", type=int, default=BUDGET, help="largest allowed count")
    args = parser.parse_args(argv)
    try:
        names = pr_workflows(args.dir)
    except (OSError, yaml.YAMLError) as exc:
        print(f"pr_workflow_budget: cannot read {args.dir}: {exc}", file=sys.stderr)
        return 2
    for name in names:
        print(f"  {name}")
    print(f"{len(names)} pull-request workflows, budget {args.budget}")
    if len(names) > args.budget:
        print(
            f"pr_workflow_budget: {len(names) - args.budget} over budget; fold the new lane into an"
            " existing workflow as a job, or call it from pr-label-lanes.yml",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
