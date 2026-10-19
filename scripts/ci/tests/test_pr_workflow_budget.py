#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cases for scripts/ci/pr_workflow_budget.py.

Positive: the checked-in workflows are at or below the budget.
Negative: one pull-request workflow over the budget fails with exit 1; an
unreadable workflow exits 2.
Boundary: exactly the budget passes; ``on:`` read as the boolean key, as a
string, a list and a mapping; ``pull_request_target`` and other events do not
count.
"""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pr_workflow_budget as budget  # the module under test, one directory up


def write(directory: Path, name: str, on_block: str) -> None:
    (directory / name).write_text(
        f"name: {name}\n{on_block}\njobs:\n  a:\n    runs-on: ubuntu-26.04\n"
        "    steps:\n      - run: 'true'\n",
        encoding="utf-8",
    )


def run(directory: Path, limit: int) -> int:
    with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
        return int(budget.main(["--dir", str(directory), "--budget", str(limit)]))


class TriggerFormsTest(unittest.TestCase):
    def test_mapping_form(self) -> None:
        self.assertIn("pull_request", budget.triggers({True: {"pull_request": None}}))

    def test_string_form(self) -> None:
        self.assertEqual(budget.triggers({"on": "pull_request"}), ["pull_request"])

    def test_list_form(self) -> None:
        self.assertIn("pull_request", budget.triggers({True: ["push", "pull_request"]}))

    def test_no_on_key(self) -> None:
        self.assertEqual(budget.triggers({"name": "x"}), [])
        self.assertEqual(budget.triggers(None), [])


class BudgetTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def fill(self, count: int) -> None:
        forms = ("on:\n  pull_request:\n", "on: pull_request", "on: [push, pull_request]")
        for i in range(count):
            write(self.dir, f"w{i:02d}.yml", forms[i % len(forms)])

    # --- positive -------------------------------------------------------

    def test_checked_in_workflows_fit(self) -> None:
        names = budget.pr_workflows(budget.WORKFLOW_DIR)
        self.assertLessEqual(len(names), budget.BUDGET, names)

    # --- boundary -------------------------------------------------------

    def test_exactly_the_budget_passes(self) -> None:
        self.fill(budget.BUDGET)
        self.assertEqual(run(self.dir, budget.BUDGET), 0)

    def test_other_events_do_not_count(self) -> None:
        self.fill(budget.BUDGET)
        write(self.dir, "target.yml", "on:\n  pull_request_target:\n")
        write(self.dir, "push.yml", "on:\n  push:\n")
        write(self.dir, "notes.txt", "on: pull_request")
        self.assertEqual(run(self.dir, budget.BUDGET), 0)

    def test_yaml_extension_counts(self) -> None:
        write(self.dir, "a.yaml", "on: pull_request")
        self.assertEqual(budget.pr_workflows(self.dir), ["a.yaml"])

    # --- planted negatives ----------------------------------------------

    def test_one_over_the_budget_fails(self) -> None:
        self.fill(budget.BUDGET + 1)
        self.assertEqual(run(self.dir, budget.BUDGET), 1)

    def test_unreadable_workflow_exits_2(self) -> None:
        (self.dir / "bad.yml").write_text("on: [pull_request\n", encoding="utf-8")
        self.assertEqual(run(self.dir, budget.BUDGET), 2)


if __name__ == "__main__":
    unittest.main()
