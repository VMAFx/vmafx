#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A master push aggregator judges the push, not the pull request on the same commit.

The merge train lands a pull request by fast-forward, so the pushed commit is
also the pull request's head, and the pull-request runs it cancels after
landing sit on that SHA. The aggregator read those cancelled runs of the
pull-request-only gates as failures of every master push
(T-CI-AGGREGATOR-READS-OTHER-EVENT-CHECKS-2026-10-06). These cases run the real
embedded script of .github/workflows/required-aggregator.yml.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci.required_aggregator_harness import run_required_aggregator

PR_ONLY_GATE = "Deliverables Checklist"
PUSH_SUITE = 1
PR_SUITE = 7
OTHER_BRANCH_SUITE = 8
SCHEDULE_SUITE = 9
CODE_SCANNING_SUITE = 99
RUNS_ON_LANDED_COMMIT = (
    {"event": "push", "head_branch": "master", "check_suite_id": PUSH_SUITE},
    {"event": "pull_request", "head_branch": "fix/example", "check_suite_id": PR_SUITE},
)
# release-please opens its release-notes branch at the master head; its push
# runs of the workflows without a branch filter sit on the same commit.
RELEASE_NOTES_BRANCH = "release-please--branches--master--components--vmafx--release-notes"
RUNS_WITH_ANOTHER_BRANCH = (
    *RUNS_ON_LANDED_COMMIT,
    {"event": "push", "head_branch": RELEASE_NOTES_BRANCH, "check_suite_id": OTHER_BRANCH_SUITE},
)
# A scheduled run of a workflow that also reports a required context on push lands on the
# master head while the push aggregator still polls; its gate job skips on schedule.
RUNS_WITH_A_SCHEDULE = (
    *RUNS_ON_LANDED_COMMIT,
    {"event": "schedule", "head_branch": "master", "check_suite_id": SCHEDULE_SUITE},
)
SCHEDULED_GATE = "FFmpeg Patch Stack"
SCHEDULED_SKIP = {
    "name": SCHEDULED_GATE,
    "conclusion": "skipped",
    "check_suite": {"id": SCHEDULE_SUITE},
    "started_at": "2099-01-01T00:00:00Z",  # after the push's own check run
}
# A required context outside strictMustReport that reports on a master push. It was `Go API
# Compatibility` while praetor's workflow pushed on every branch; that gate is strict now.
API_GATE = "Semgrep OSS"


class AggregatorEventScope(unittest.TestCase):
    def test_push_ignores_cancelled_pull_request_runs(self) -> None:
        failures = run_required_aggregator(
            PR_ONLY_GATE,
            "cancelled",
            event="push",
            selected_suite=PR_SUITE,
            workflow_runs=RUNS_ON_LANDED_COMMIT,
        )
        self.assertEqual(failures, [])

    def test_push_still_fails_its_own_cancelled_run(self) -> None:
        failures = run_required_aggregator(
            PR_ONLY_GATE,
            "cancelled",
            event="push",
            selected_suite=PUSH_SUITE,
            workflow_runs=RUNS_ON_LANDED_COMMIT,
        )
        self.assertEqual(len(failures), 1)
        self.assertIn(f"{PR_ONLY_GATE}: cancelled", failures[0])

    def test_pull_request_still_reads_its_own_runs(self) -> None:
        failures = run_required_aggregator(
            PR_ONLY_GATE,
            "cancelled",
            event="pull_request",
            selected_suite=PR_SUITE,
            workflow_runs=RUNS_ON_LANDED_COMMIT,
        )
        self.assertEqual(len(failures), 1)
        self.assertIn(f"{PR_ONLY_GATE}: cancelled", failures[0])

    def test_push_still_reads_check_runs_of_other_apps(self) -> None:
        """Code scanning reports from its own suite, which no workflow run owns."""
        failures = run_required_aggregator(
            "CodeQL",
            "failure",
            event="push",
            selected_suite=CODE_SCANNING_SUITE,
            workflow_runs=RUNS_ON_LANDED_COMMIT,
        )
        self.assertEqual(len(failures), 1)
        self.assertIn("CodeQL: failure", failures[0])

    def test_push_ignores_cancelled_runs_on_another_branch(self) -> None:
        """T-CI-AGGREGATOR-READS-OTHER-BRANCH-RUNS-2026-10-06 (master 6275c6f1a)."""
        failures = run_required_aggregator(
            API_GATE,
            "cancelled",
            event="push",
            selected_suite=OTHER_BRANCH_SUITE,
            workflow_runs=RUNS_WITH_ANOTHER_BRANCH,
        )
        self.assertEqual(failures, [])

    def test_push_still_fails_its_own_cancelled_run_beside_another_branch(self) -> None:
        failures = run_required_aggregator(
            API_GATE,
            "cancelled",
            event="push",
            selected_suite=PUSH_SUITE,
            workflow_runs=RUNS_WITH_ANOTHER_BRANCH,
        )
        self.assertEqual(len(failures), 1)
        self.assertIn(f"{API_GATE}: cancelled", failures[0])

    def test_pull_request_reads_runs_of_every_branch(self) -> None:
        """Outside a push, nothing changes: a pull request reads every suite."""
        failures = run_required_aggregator(
            API_GATE,
            "cancelled",
            event="pull_request",
            selected_suite=OTHER_BRANCH_SUITE,
            workflow_runs=RUNS_WITH_ANOTHER_BRANCH,
        )
        self.assertEqual(len(failures), 1)
        self.assertIn(f"{API_GATE}: cancelled", failures[0])

    def test_push_ignores_a_skipped_scheduled_run(self) -> None:
        """T-CI-AGGREGATOR-READS-SCHEDULE-RUNS-2026-10-15 (master fdf711ba6, run 38545862225)."""
        # The push run succeeded; the scheduled run's skipped check run is newer.
        failures = run_required_aggregator(
            SCHEDULED_GATE,
            "success",
            event="push",
            selected_suite=PUSH_SUITE,
            workflow_runs=RUNS_WITH_A_SCHEDULE,
            extra_checks=[SCHEDULED_SKIP],
        )
        self.assertEqual(failures, [])

    def test_push_still_fails_its_own_skipped_run_beside_a_schedule(self) -> None:
        failures = run_required_aggregator(
            SCHEDULED_GATE,
            "skipped",
            event="push",
            selected_suite=PUSH_SUITE,
            workflow_runs=RUNS_WITH_A_SCHEDULE,
            extra_checks=[{**SCHEDULED_SKIP, "conclusion": "success"}],
        )
        self.assertEqual(len(failures), 1)
        self.assertIn(f"{SCHEDULED_GATE}: skipped", failures[0])


if __name__ == "__main__":
    unittest.main()
