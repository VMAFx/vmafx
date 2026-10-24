#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cases for scripts/ci/check_job_timeout_budget.py.

Positive: the checked-in workflows hold; a job above its step sum passes.
Negative: the planted job of 2026-10-22's shape with its limit below the step
sum fails with exit 1; a job without a limit whose steps pass GitHub's 360
minutes fails; an expression or a negative limit exits 2; an unreadable
workflow exits 2.
Negative: an apt-get install step without its own cap fails with exit 1, the
planted 2026-10-23 Coverage Gate shape; an expired exception no longer exempts.
Boundary: an unexpired exception exempts its file only; a limit equal to the step sum passes; steps without a cap add
nothing; a reusable-workflow call job (no steps) is skipped.
"""

from __future__ import annotations

import datetime
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_job_timeout_budget as budget  # the module under test, one directory up


def job(limit: str, *step_caps: str) -> str:
    """A workflow with one job; ``limit`` and each step cap are YAML scalars, "" for none."""
    head = "    timeout-minutes: " + limit + "\n" if limit else ""
    steps = "".join(
        "      - run: 'true'\n" + ("        timeout-minutes: " + cap + "\n" if cap else "")
        for cap in step_caps
    )
    return (
        "on: pull_request\njobs:\n  a:\n    runs-on: ubuntu-26.04\n" + head + "    steps:\n" + steps
    )


class JobTimeoutBudgetTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_on(self, text: str) -> int:
        (self.dir / "w.yml").write_text(text, encoding="utf-8")
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return int(budget.main(["--dir", str(self.dir)]))

    def test_checked_in_workflows_hold(self) -> None:
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(budget.main([]), 0)

    def test_planted_limit_below_step_sum_is_refused(self) -> None:
        # The apt step's 20 minutes plus a capped 24-minute build in a 35-minute job.
        self.assertEqual(self.run_on(job("35", "20", "24")), 1)

    def test_finding_names_the_job_and_both_numbers(self) -> None:
        doc = {"jobs": {"mcp": {"timeout-minutes": 35, "steps": [{"timeout-minutes": 40}]}}}
        self.assertEqual(
            budget.job_findings("w.yml", doc),
            ["w.yml: job mcp: timeout-minutes 35 is below its step caps' sum 40"],
        )

    def test_limit_above_step_sum_passes(self) -> None:
        self.assertEqual(self.run_on(job("55", "20", "")), 0)

    def test_limit_equal_to_step_sum_passes(self) -> None:
        self.assertEqual(self.run_on(job("30", "20", "10")), 0)

    def test_uncapped_steps_add_nothing(self) -> None:
        self.assertEqual(self.run_on(job("5", "", "", "")), 0)

    def test_missing_job_limit_is_github_default(self) -> None:
        self.assertEqual(self.run_on(job("", "300", "60")), 0)
        self.assertEqual(self.run_on(job("", "300", "61")), 1)

    def test_expression_limit_fails_closed(self) -> None:
        self.assertEqual(self.run_on(job("${{ inputs.minutes }}", "20")), 2)
        self.assertEqual(self.run_on(job("55", "${{ inputs.minutes }}")), 2)

    def test_negative_and_boolean_limits_fail_closed(self) -> None:
        self.assertEqual(self.run_on(job("-1", "20")), 2)
        self.assertEqual(self.run_on(job("true", "20")), 2)

    def test_reusable_workflow_call_is_skipped(self) -> None:
        text = "on: push\njobs:\n  a:\n    uses: ./.github/workflows/x.yml\n"
        self.assertEqual(self.run_on(text), 0)

    def test_planted_uncapped_apt_step_is_refused(self) -> None:
        # The 2026-10-23 Coverage Gate: libcudart12 fetched for 30 minutes in an uncapped step.
        text = job("75", "", "") + "      - run: sudo apt-get install -y libcudart12\n"
        self.assertEqual(self.run_on(text), 1)

    def test_capped_apt_step_passes(self) -> None:
        text = (
            job("75", "")
            + "      - run: sudo apt-get install -y libcudart12\n        timeout-minutes: 35\n"
        )
        self.assertEqual(self.run_on(text), 0)

    def test_uncapped_apt_finding_names_the_step(self) -> None:
        doc = {"jobs": {"cov": {"steps": [{"name": "deps", "run": "apt-get install -y x"}]}}}
        self.assertEqual(
            budget.job_findings("w.yml", doc),
            ["w.yml: job cov, deps: apt-get install step has no timeout-minutes"],
        )

    def test_apt_exception_holds_until_expiry_only(self) -> None:
        # A stand-in entry: the real list shrinks with each capped file.
        doc = {"jobs": {"b": {"steps": [{"name": "deps", "run": "apt-get install -y x"}]}}}
        expiry = datetime.date(2026, 10, 26)
        with mock.patch.dict(budget.UNCAPPED_APT_EXCEPTIONS, {"listed.yml": expiry}):
            self.assertEqual(budget.job_findings("listed.yml", doc, expiry), [])
            late = expiry + datetime.timedelta(days=1)
            self.assertEqual(len(budget.job_findings("listed.yml", doc, late)), 1)
            self.assertEqual(len(budget.job_findings("other.yml", doc, expiry)), 1)

    def test_unreadable_workflow_exits_2(self) -> None:
        self.assertEqual(self.run_on("jobs: [unclosed\n"), 2)


if __name__ == "__main__":
    unittest.main()
