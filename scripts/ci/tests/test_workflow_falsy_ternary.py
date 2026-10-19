#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""No workflow expression picks a falsy literal with `cond && X || Y`.

GitHub Actions expressions have no ternary operator; `cond && X || Y` stands in
for one only while X is truthy. `0`, `''`, `false` and `null` are falsy, so
`cond && 0 || 1` is 1 whatever `cond` is. Two checkouts used that form for
`fetch-depth` (libvmaf-build-matrix.yml, #2390; lint-and-format.yml Docs, #2421):
every leg cloned one commit, `git describe` found no tag, `vmaf -v` printed the
tagless fallback and `test_vmaf_version_unaffected` failed on every Ubuntu tox
leg, and the Docs render check reported a shallow history. Write the negated
condition first instead: `!cond && 1 || 0`.

Positive, negative and boundary cases run against synthetic lines; the last
test scans every tracked workflow and composite action.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import ci_expressions as gx  # noqa: E402

# One `${{ ... }}` expression body; prose in YAML comments is not an expression.
EXPRESSION = re.compile(r"\$\{\{(.*?)\}\}")
# `&&` then a falsy literal then `||`, inside one expression.
FALSY_TERNARY = re.compile(r"&&\s*(?:0|-0|''|\"\"|false|null)\s*\|\|")


def falsy_ternaries(text: str) -> list[int]:
    """1-based line numbers holding an expression that uses `&& <falsy> ||`."""
    return [
        n
        for n, line in enumerate(text.splitlines(), 1)
        if any(FALSY_TERNARY.search(body) for body in EXPRESSION.findall(line))
    ]


class FalsyTernary(unittest.TestCase):
    def test_negated_form_passes(self) -> None:
        self.assertEqual(falsy_ternaries("fetch-depth: ${{ x != 'true' && 1 || 0 }}"), [])

    def test_truthy_string_passes(self) -> None:
        self.assertEqual(falsy_ternaries("a: ${{ x && '0' || '1' }}"), [])

    def test_prose_outside_expression_passes(self) -> None:
        self.assertEqual(falsy_ternaries("# Not `run && 0 || 1`: 0 is falsy."), [])

    def test_zero_fails(self) -> None:
        self.assertEqual(falsy_ternaries("a: 1\nfetch-depth: ${{ x == 'true' && 0 || 1 }}"), [2])

    def test_other_falsy_literals_fail(self) -> None:
        for literal in ("''", '""', "false", "null", "-0"):
            with self.subTest(literal=literal):
                self.assertEqual(falsy_ternaries(f"a: ${{{{ c && {literal} || 'x' }}}}"), [1])

    def test_boundary_numbers_starting_with_zero_pass(self) -> None:
        self.assertEqual(falsy_ternaries("a: ${{ c && 0.5 || 1 }}"), [])
        self.assertEqual(falsy_ternaries("a: ${{ c && 10 || 1 }}"), [])

    def test_tracked_workflows_and_actions(self) -> None:
        files = sorted((ROOT / ".github" / "workflows").glob("*.y*ml"))
        files += sorted((ROOT / ".github" / "actions").glob("**/action.y*ml"))
        self.assertTrue(files)
        offenders = {}
        for path in files:
            lines = falsy_ternaries(path.read_text(encoding="utf-8"))
            if lines:
                offenders[str(path.relative_to(ROOT))] = lines
        self.assertEqual(offenders, {}, "write `!cond && X || Y` with X truthy")


# Each expression-valued `fetch-depth`, with the context that must clone the
# whole history (0) and the one that may clone one commit (1).
FETCH_DEPTH_CASES: dict[str, tuple[dict[str, Any], dict[str, Any]]] = {
    ".github/workflows/libvmaf-build-matrix.yml": (
        {"steps": {"leg": {"outputs": {"run": "true"}}}},
        {"steps": {"leg": {"outputs": {"run": "false"}}}},
    ),
    ".github/workflows/lint-and-format.yml": (
        {"github": {"event_name": "push"}},
        {"github": {"event_name": "pull_request"}},
    ),
}
FETCH_DEPTH = re.compile(r"^\s*fetch-depth:\s*(\$\{\{.*\}\})\s*$", re.MULTILINE)


class FetchDepthSemantics(unittest.TestCase):
    """Evaluate the real expressions with scripts/ci/ci_expressions.py (ADR-2169)."""

    def test_old_form_always_picks_one(self) -> None:
        for run in ("true", "false"):
            with self.subTest(run=run):
                context = {"x": run}
                self.assertEqual(gx.evaluate("${{ x == 'true' && 0 || 1 }}", context), 1)

    def test_every_expression_depth_is_covered(self) -> None:
        found = {
            str(path.relative_to(ROOT))
            for path in (ROOT / ".github").glob("**/*.y*ml")
            if FETCH_DEPTH.search(path.read_text(encoding="utf-8"))
        }
        self.assertEqual(found, set(FETCH_DEPTH_CASES))

    def test_full_history_where_intended(self) -> None:
        for name, (full, shallow) in FETCH_DEPTH_CASES.items():
            for expression in FETCH_DEPTH.findall((ROOT / name).read_text(encoding="utf-8")):
                with self.subTest(file=name, expression=expression):
                    self.assertEqual(gx.evaluate(expression, full), 0)
                    self.assertEqual(gx.evaluate(expression, shallow), 1)


if __name__ == "__main__":
    unittest.main()
