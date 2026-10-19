# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""The checks of praetor's workflows are required aggregator checks that cannot wait forever.

`Go API Compatibility` (praetor-api.yml, ADR-1506) and `REUSE lint` (reuse.yml, ADR-2784).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
# (check name, workflow that reports it)
CHECKS = (
    ("Go API Compatibility", "praetor-api.yml"),
    ("REUSE lint", "reuse.yml"),
)


def _list(text: str, start: str) -> set[str]:
    body = re.search(rf"const {start} = \[(.*?)\];", text, re.DOTALL)
    assert body is not None
    return set(re.findall(r"'([^']+)'", re.sub(r"(?m)^\s*//.*$", "", body.group(1))))


class PraetorChecksRequiredTests(unittest.TestCase):
    def setUp(self) -> None:
        self.aggregator = (ROOT / ".github/workflows/required-aggregator.yml").read_text()

    def test_required_and_strict(self) -> None:
        for name, _ in CHECKS:
            with self.subTest(name=name):
                self.assertIn(name, _list(self.aggregator, "required"))
                # The workflow listens for ready_for_review since praetor#815, so a draft that
                # becomes ready is re-run and a missing result is a broken workflow, never a skip
                # (ADR-2440).
                self.assertIn(name, _list(self.aggregator, "strictMustReport"))

    def test_workflow_reports_on_every_pull_request(self) -> None:
        for name, workflow in CHECKS:
            with self.subTest(name=name):
                text = (ROOT / ".github/workflows" / workflow).read_text()
                self.assertIn(f"name: {name}", text)
                self.assertIsNone(re.search(r"(?m)^\s*paths(-ignore)?:", text))
                # A job-level condition would skip the required context; the draft stop is a
                # step (praetor#815).
                self.assertIsNone(re.search(r"(?m)^ {4}if:", text))
                self.assertIn("ready_for_review", text)


if __name__ == "__main__":
    unittest.main()
