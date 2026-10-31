#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every CI job that runs a test reading a fixture bundle fetches that bundle first.

A Python test that reads a bundle under `python/test/resource/yuv/<bundle>` (named in
the test as `"yuv", "<name>-v<N>"`) only finds it when the job ran the
`./.github/actions/pq-fixture` action with `phase: get` for that bundle. Without it the
test asks the upstream resource host, which does not hold our bundles, and fails with
HTTP 404 (or, with `bypass_download=True`, with a missing file).

A job runs a test file when one of its `run:` steps, or a `scripts/ci/*.sh` script a
step calls, names the whole suite (`python/test/` or `tox -c python`) or the file's
own name. Usage: check_fixture_bundle_jobs.py [--root DIR]; exit 1 lists each gap.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment

BUNDLE_REF = re.compile(r"""["']yuv["']\s*,\s*["']([a-z0-9]+(?:-[a-z0-9]+)*-v\d+)["']""")
WHOLE_SUITE = re.compile(r"(python/test/?(?=[\s\"'\\]|$)|tox\s+-c\s+python\b)", re.MULTILINE)
CALLED_SCRIPT = re.compile(r"\b(scripts/ci/[\w./-]+\.sh)\b")
FIXTURE_ACTION = "./.github/actions/pq-fixture"
DEFAULT_BUNDLE = "pq-sollevante-v1"
MAX_STEPS = 500


def bundle_tests(root: Path) -> dict[str, set[str]]:
    """Map each test file stem under python/test to the fixture bundles it reads."""
    found: dict[str, set[str]] = {}
    for path in sorted((root / "python" / "test").glob("*.py")):
        names = set(BUNDLE_REF.findall(path.read_text(encoding="utf-8")))
        if names:
            found[path.stem] = names
    return found


def job_commands(root: Path, steps: list[dict[str, Any]]) -> str:
    """The job's run text, with every scripts/ci shell script it calls appended."""
    runs = [str(step.get("run", "")) for step in steps[:MAX_STEPS]]
    text = "\n".join(runs)
    for script in sorted(set(CALLED_SCRIPT.findall(text))):
        path = root / script
        if path.is_file():
            runs.append(path.read_text(encoding="utf-8"))
    return "\n".join(runs)


def fetched_bundles(steps: list[dict[str, Any]]) -> set[str]:
    """Bundles the job fetches through the fixture action before its tests."""
    fetched: set[str] = set()
    for step in steps[:MAX_STEPS]:
        params = step.get("with") or {}
        if step.get("uses") == FIXTURE_ACTION and params.get("phase") == "get":
            fetched.add(str(params.get("bundle", DEFAULT_BUNDLE)))
    return fetched


def job_gaps(root: Path, steps: list[dict[str, Any]], tests: dict[str, set[str]]) -> list[str]:
    """Bundles a job's tests read but the job never fetches, as 'bundle (test)' strings."""
    commands = job_commands(root, steps)
    fetched = fetched_bundles(steps)
    whole_suite = WHOLE_SUITE.search(commands) is not None
    gaps: list[str] = []
    for stem, bundles in sorted(tests.items()):
        if not (whole_suite or re.search(rf"\b{re.escape(stem)}\b", commands)):
            continue
        gaps.extend(f"{bundle} ({stem})" for bundle in sorted(bundles - fetched))
    return gaps


def check(root: Path) -> list[str]:
    """Every gap across the workflows, as 'workflow:job: bundle (test)' lines."""
    tests = bundle_tests(root)
    problems: list[str] = []
    for workflow in sorted((root / ".github" / "workflows").glob("*.yml")):
        jobs = (yaml.safe_load(workflow.read_text(encoding="utf-8")) or {}).get("jobs") or {}
        for job_id, job in jobs.items():
            steps = (job or {}).get("steps") or []
            for gap in job_gaps(root, steps, tests):
                problems.append(f"{workflow.name}:{job_id}: does not fetch {gap}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check that CI jobs fetch the fixture bundles their tests read."
    )
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    problems = check(args.root)
    for line in problems:
        print(f"check_fixture_bundle_jobs: {line}", file=sys.stderr)
    if problems:
        print(
            f"check_fixture_bundle_jobs: add a '{FIXTURE_ACTION}' step with phase: get "
            "for each bundle before the tests run",
            file=sys.stderr,
        )
        return 1
    print("check_fixture_bundle_jobs: every job that reads a fixture bundle fetches it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
