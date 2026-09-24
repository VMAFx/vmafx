#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise the real required-check script with Go outcomes and routing guards."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.required_aggregator_harness import run_required_aggregator

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
GO_CHECK = "go vet + go test"


class GoWorkflowContract(unittest.TestCase):
    def test_ready_pr_and_master_runs_are_routed_inside_the_job(self) -> None:
        workflow = (WORKFLOWS / "go-ci.yml").read_text(encoding="utf-8")
        trigger = workflow.split("\nconcurrency:", 1)[0]
        self.assertNotRegex(trigger, r"(?m)^\s+paths(?:-ignore)?:")
        self.assertIn("types: [opened, synchronize, reopened, ready_for_review]", trigger)
        self.assertIn("branches: [master]", trigger)
        self.assertIn("github.event.pull_request.draft == false", workflow)
        self.assertIn("fetch-depth: 0", workflow)
        self.assertIn("name: " + GO_CHECK, workflow)
        self.assertIn("# required-aggregator", workflow)
        self.assertIn('--base "$BASE_SHA" --head "$HEAD_SHA"', workflow)
        steps = workflow.split("\n      - ")[1:]
        heavy = [
            step
            for step in steps
            if not step.startswith(
                ("uses: actions/checkout@", "name: Plan CI", "name: Go checks not impacted")
            )
        ]
        self.assertGreater(len(heavy), 8)
        for step in heavy:
            self.assertIn("if: steps.impact.outputs.go_checks == 'true'", step)
            self.assertNotIn("continue-on-error", step)
        self.assertIn("if: steps.impact.outputs.go_checks != 'true'", workflow)

    def test_failed_go_scan_blocks_otherwise_green_checks(self) -> None:
        failures = run_required_aggregator(GO_CHECK, "failure")
        self.assertEqual(len(failures), 1)
        self.assertIn(GO_CHECK + ": failure", failures[0])

    def test_successful_go_checks_pass(self) -> None:
        self.assertEqual(run_required_aggregator(GO_CHECK, "success"), [])

    def test_unreported_check_keeps_existing_absence_semantics(self) -> None:
        self.assertEqual(run_required_aggregator(GO_CHECK, None), [])


if __name__ == "__main__":
    unittest.main()
