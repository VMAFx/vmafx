#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every CI job that runs a test reading a fixture bundle fetches that bundle first.

A Python test that reads a bundle under `python/test/resource/yuv/<bundle>` (named in
the test as `"yuv", "<name>-v<N>"`) only finds it when the job ran the
`./.github/actions/pq-fixture` action with `phase: get` for that bundle. Without it the
test asks the upstream resource host, which does not hold our bundles, and fails with
HTTP 404 (or, with `bypass_download=True`, with a missing file).

A step runs a test file when its `run:` text, or a `scripts/ci/*.sh` script it calls,
names the whole suite (`python/test/` or `tox -c python`) or the file's own name. The
get step for each bundle that file reads must come earlier in the job, under an `if:`
the test step's `if:` implies (checked by truth table over the atomic predicates), and must name a bundle in the `SUPPORTED_BUNDLES` line
of `scripts/test/fetch-test-yuvs.sh`; the action cannot fetch any other. A `phase:
finish` step must run after a failed test too, so its `if:` starts with `!cancelled()`
or `always()`; otherwise a red test leaves the fixture in the shared resource cache.
Usage: check_fixture_bundle_jobs.py [--root DIR]; exit 1 lists each gap.
"""

from __future__ import annotations

import argparse
import itertools
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
FETCH_SCRIPT = "scripts/test/fetch-test-yuvs.sh"
SUPPORTED_LINE = re.compile(r"^SUPPORTED_BUNDLES=\(([^)]*)\)", re.MULTILINE)
RUNS_AFTER_FAILURE = re.compile(r"^\s*(\$\{\{\s*)?(!\s*cancelled\(\)|always\(\))")
MAX_STEPS = 500
MAX_ATOMS = 12
# One token of a GitHub expression: an operator, a parenthesis, or an atomic predicate
# (a function call, a comparison, or a bare context path).
TOKEN = re.compile(
    r"\s*(&&|\|\||!(?!=)|\(|\)"
    r"|[\w.-]+\([^()]*\)(?:\s*[!=]=\s*(?:'[^']*'|[\w.-]+))?"
    r"|[\w.-]+\s*[!=]=\s*(?:'[^']*'|[\w.-]+)"
    r"|[\w.-]+)"
)
PRECEDENCE = {"!": 3, "&&": 2, "||": 1}


def _push_operator(tok: str, out: list[str], ops: list[str]) -> bool:
    """Shunting-yard step for one parenthesis or operator; False on an unmatched ')'."""
    if tok == "(":
        ops.append(tok)
        return True
    if tok == ")":
        while ops and ops[-1] != "(":
            out.append(ops.pop())
        if not ops:
            return False
        ops.pop()
        return True
    while ops and ops[-1] != "(" and tok != "!" and PRECEDENCE[ops[-1]] >= PRECEDENCE[tok]:
        out.append(ops.pop())
    ops.append(tok)
    return True


def to_rpn(condition: str) -> list[str] | None:
    """The condition in postfix order (shunting-yard, no recursion); None if it does not parse."""
    text = condition.strip()
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2]
    text = text.rstrip()
    out: list[str] = []
    ops: list[str] = []
    pos = 0
    while pos < len(text):
        match = TOKEN.match(text, pos)
        if match is None:
            return None
        pos, tok = match.end(), match.group(1)
        if tok in PRECEDENCE or tok in "()":
            if not _push_operator(tok, out, ops):
                return None
        else:
            out.append(re.sub(r"\s+", "", tok))
    return None if "(" in ops else out + ops[::-1]


def evaluate(rpn: list[str], values: dict[str, bool]) -> bool:
    """The postfix condition's value with each atom set as in values."""
    stack: list[bool] = []
    for tok in rpn:
        if tok == "!":
            stack.append(not stack.pop())
        elif tok in ("&&", "||"):
            right, left = stack.pop(), stack.pop()
            stack.append(left and right if tok == "&&" else left or right)
        else:
            stack.append(values[tok])
    return stack[-1] if len(stack) == 1 else False


def implies(test_if: str, fetch_if: str) -> bool:
    """True when every case that runs the test step also runs the fetch step."""
    if not fetch_if.strip() or test_if.strip() == fetch_if.strip():
        return True
    test = to_rpn(test_if) if test_if.strip() else []
    fetch = to_rpn(fetch_if)
    if test is None or fetch is None:
        return False
    atoms = sorted({t for t in test + fetch if t not in PRECEDENCE})
    if len(atoms) > MAX_ATOMS:
        return False
    for row in itertools.product((False, True), repeat=len(atoms)):
        values = dict(zip(atoms, row, strict=True))
        if (not test or evaluate(test, values)) and not evaluate(fetch, values):
            return False
    return True


def bundle_tests(root: Path) -> dict[str, set[str]]:
    """Map each test file stem under python/test to the fixture bundles it reads."""
    found: dict[str, set[str]] = {}
    for path in sorted((root / "python" / "test").glob("*.py")):
        names = set(BUNDLE_REF.findall(path.read_text(encoding="utf-8")))
        if names:
            found[path.stem] = names
    return found


def supported_bundles(root: Path) -> set[str]:
    """The bundle names in the fetch script's SUPPORTED_BUNDLES line; empty if it has none."""
    path = root / FETCH_SCRIPT
    match = SUPPORTED_LINE.search(path.read_text(encoding="utf-8")) if path.is_file() else None
    return set(re.findall(r'"([^"]+)"', match.group(1))) if match else set()


def step_commands(root: Path, step: dict[str, Any]) -> str:
    """The step's run text, with every scripts/ci shell script it calls appended."""
    runs = [str(step.get("run", ""))]
    for script in sorted(set(CALLED_SCRIPT.findall(runs[0]))):
        path = root / script
        if path.is_file():
            runs.append(path.read_text(encoding="utf-8"))
    return "\n".join(runs)


def _fetch_bundle(step: dict[str, Any]) -> str | None:
    """The bundle a fixture get step fetches, or None for any other step."""
    params = step.get("with") or {}
    if step.get("uses") == FIXTURE_ACTION and params.get("phase") == "get":
        return str(params.get("bundle", DEFAULT_BUNDLE))
    return None


def _tests_run(commands: str, tests: dict[str, set[str]]) -> list[tuple[str, str]]:
    """(test stem, bundle) pairs for the bundle tests a step's commands run."""
    whole_suite = WHOLE_SUITE.search(commands) is not None
    return [
        (stem, bundle)
        for stem, bundles in sorted(tests.items())
        if whole_suite or re.search(rf"\b{re.escape(stem)}\b", commands)
        for bundle in sorted(bundles)
    ]


def job_gaps(
    root: Path, steps: list[dict[str, Any]], tests: dict[str, set[str]], supported: set[str]
) -> list[str]:
    """Each fetch a job's tests miss, in step order, as problem text without the job prefix."""
    fetched: dict[str, str] = {}
    gaps: list[str] = []
    for step in steps[:MAX_STEPS]:
        bundle = _fetch_bundle(step)
        if bundle is not None:
            if bundle in supported:
                fetched[bundle] = str(step.get("if", ""))
            else:
                gaps.append(f"fetches {bundle}, which {FETCH_SCRIPT} does not support")
            continue
        condition = str(step.get("if", ""))
        for stem, needed in _tests_run(step_commands(root, step), tests):
            got = fetched.get(needed)
            if got is None or not implies(condition, got):
                gap = f"does not fetch {needed} ({stem}) before the step, under its condition"
                if gap not in gaps:
                    gaps.append(gap)
    return gaps


def finish_steps_skipped_on_failure(steps: list[dict[str, Any]]) -> list[str]:
    """Names of finish steps that a failed earlier step would skip."""
    skipped: list[str] = []
    for index, step in enumerate(steps[:MAX_STEPS]):
        params = step.get("with") or {}
        if step.get("uses") != FIXTURE_ACTION or params.get("phase") != "finish":
            continue
        if not RUNS_AFTER_FAILURE.match(str(step.get("if", ""))):
            skipped.append(str(step.get("name", f"step {index + 1}")))
    return skipped


def check(root: Path) -> list[str]:
    """Every gap across the workflows, as 'workflow:job: bundle (test)' lines."""
    tests = bundle_tests(root)
    supported = supported_bundles(root)
    if not supported:
        return [f"{FETCH_SCRIPT}: no SUPPORTED_BUNDLES line to read the fetchable bundles from"]
    problems: list[str] = []
    for workflow in sorted((root / ".github" / "workflows").glob("*.yml")):
        jobs = (yaml.safe_load(workflow.read_text(encoding="utf-8")) or {}).get("jobs") or {}
        for job_id, job in jobs.items():
            steps = (job or {}).get("steps") or []
            for gap in job_gaps(root, steps, tests, supported):
                problems.append(f"{workflow.name}:{job_id}: {gap}")
            for name in finish_steps_skipped_on_failure(steps):
                problems.append(
                    f"{workflow.name}:{job_id}: '{name}' is skipped after a failed step; "
                    "start its if: with !cancelled()"
                )
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
            f"check_fixture_bundle_jobs: a fetch gap needs a '{FIXTURE_ACTION}' step with "
            "phase: get before the tests; a finish step needs if: !cancelled()",
            file=sys.stderr,
        )
        return 1
    print("check_fixture_bundle_jobs: every job that reads a fixture bundle fetches it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
