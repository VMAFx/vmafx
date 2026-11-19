#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A master push aggregator checks the push, not the pull request on the same commit.

The merge train lands a pull request by fast-forward, so the pushed commit is
also the pull request's head, and the pull-request runs it cancels after
landing sit on that SHA. The aggregator read those cancelled runs of the
pull-request-only gates as failures of every master push
(T-CI-AGGREGATOR-READS-OTHER-EVENT-CHECKS-2026-10-06). These cases run the real
embedded script of .github/workflows/required-aggregator.yml.
"""

from __future__ import annotations

import contextlib
import os
import sys
import unittest
from collections.abc import Collection, Mapping, Sequence
from datetime import UTC, datetime, timedelta
from pathlib import Path
from types import MappingProxyType
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import required_aggregator_harness as harness
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

CODE_SCANNING_APP: Mapping[str, str] = MappingProxyType({"slug": "github-advanced-security"})
ACTIONS_APP: dict[str, str] = {"slug": "github-actions"}
FOREIGN_APP: dict[str, str] = {"slug": "sourcery-ai"}
EMPTY_APP: dict[str, str] = {}
UNATTRIBUTED_APPS: tuple[dict[str, str] | None, ...] = (None, EMPTY_APP)
CODE_SCANNING_CHECKS = ("CodeQL", "Semgrep OSS", "gitleaks")
UPLOAD_JOB_PAIRS = (
    ("CodeQL", "CodeQL (C/C++)"),
    ("CodeQL", "CodeQL (Python)"),
    ("CodeQL", "CodeQL (Actions)"),
    ("Semgrep OSS", "Semgrep"),
    ("gitleaks", "Gitleaks"),
)
ACTIONS_CHECK = "Pre-Commit"
# VirtualDate in required_aggregator_harness._NODE_DRIVER advances by 3 minutes per Date.now()
# call and reaches +12 minutes on the first poll's nowMs: +11m sits 1m inside the 2m propagation
# window, while +8m sits 4m behind nowMs (outside the 2m window).
WITHIN_FIRST_POLL_PROPAGATION_MIN = 11
PAST_FIRST_POLL_PROPAGATION_MIN = 8
_OVERRIDE = contextlib.ExitStack()


def setUpModule() -> None:
    """Point the harness at AGGREGATOR_UNDER_TEST when set."""
    override = os.environ.get("AGGREGATOR_UNDER_TEST")
    if override:
        _OVERRIDE.enter_context(mock.patch.object(harness, "AGGREGATOR_PATH", Path(override)))


def tearDownModule() -> None:
    """Restore any patched aggregator path."""
    _OVERRIDE.close()


def _minutes_offset(minutes: int) -> str:
    """Format a UTC timestamp shifted by ``minutes`` from now."""
    return (datetime.now(UTC) + timedelta(minutes=minutes)).strftime("%Y-%m-%dT%H:%M:%SZ")


def _minutes_ago(minutes: int) -> str:
    """Format a UTC timestamp ``minutes`` in the past."""
    return _minutes_offset(-minutes)


def _check_run(
    name: str,
    conclusion: str | None = "success",
    *,
    app: Mapping[str, str] | None = CODE_SCANNING_APP,
    suite_id: int = CODE_SCANNING_SUITE,
    status: str = "completed",
    started_at: str | None = None,
    completed_at: str | None = None,
) -> dict[str, object]:
    """Build one synthetic check-run payload with explicit producer attribution."""
    record: dict[str, object] = {
        "name": name,
        "status": status,
        "conclusion": conclusion,
        "app": dict(app) if app is not None else None,
        "check_suite": {"id": suite_id},
    }
    if started_at is not None:
        record["started_at"] = started_at
    if completed_at is not None:
        record["completed_at"] = completed_at
    return record


def _other_required_checks_succeed(excluded: Collection[str]) -> list[dict[str, object]]:
    """Return passing baseline check runs for every required name outside ``excluded``."""
    script = harness._embedded_script(harness.AGGREGATOR_PATH.read_text(encoding="utf-8"))
    excluded_names = set(excluded)
    return [
        {"name": name, "conclusion": "success", "check_suite": {"id": PUSH_SUITE}}
        for name in harness._required_names(script)
        if name not in excluded_names
    ]


def _run_pr_checks(
    absent_name: str,
    extra_checks: Sequence[Mapping[str, object]],
    *,
    absent_checks: Collection[str] = (),
    later_checks: Sequence[Mapping[str, object]] = (),
) -> list[str]:
    """Run a pull-request aggregator pass omitting ``absent_name`` from the baseline."""
    return run_required_aggregator(
        absent_name,
        None,
        event="pull_request",
        absent_checks=absent_checks,
        extra_checks=extra_checks,
        later_checks=later_checks,
    )


class _AggregatorCase(unittest.TestCase):
    """Shared assertion helpers for aggregator scenarios."""

    def assert_single_failure(self, failures: list[str], expected: str) -> None:
        """Assert that the aggregator reported one failure message containing ``expected``."""
        self.assertEqual(len(failures), 1, failures)
        self.assertIn(expected, failures[0])


class AggregatorEventScope(_AggregatorCase):
    """Event, branch and schedule check-suite scoping on push vs pull_request."""

    def test_push_ignores_cancelled_pull_request_runs(self) -> None:
        """Ignore cancelled pull-request suites during a master push."""
        failures = run_required_aggregator(
            PR_ONLY_GATE,
            "cancelled",
            event="push",
            selected_suite=PR_SUITE,
            workflow_runs=RUNS_ON_LANDED_COMMIT,
        )
        self.assertEqual(failures, [])

    def test_push_still_fails_its_own_cancelled_run(self) -> None:
        """Fail when the master push's own suite cancels a required check."""
        failures = run_required_aggregator(
            PR_ONLY_GATE,
            "cancelled",
            event="push",
            selected_suite=PUSH_SUITE,
            workflow_runs=RUNS_ON_LANDED_COMMIT,
        )
        self.assert_single_failure(failures, f"{PR_ONLY_GATE}: cancelled")

    def test_pull_request_still_reads_its_own_runs(self) -> None:
        """Fail when a pull request's own suite cancels a required check."""
        failures = run_required_aggregator(
            PR_ONLY_GATE,
            "cancelled",
            event="pull_request",
            selected_suite=PR_SUITE,
            workflow_runs=RUNS_ON_LANDED_COMMIT,
        )
        self.assert_single_failure(failures, f"{PR_ONLY_GATE}: cancelled")

    def test_push_still_reads_check_runs_of_other_apps(self) -> None:
        """Code scanning reports from its own suite, which no workflow run owns."""
        failures = run_required_aggregator(
            "CodeQL",
            "failure",
            event="push",
            selected_suite=CODE_SCANNING_SUITE,
            workflow_runs=RUNS_ON_LANDED_COMMIT,
        )
        self.assert_single_failure(failures, "CodeQL: failure")

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
        """Fail when the push's own suite is cancelled alongside another branch's suite."""
        failures = run_required_aggregator(
            API_GATE,
            "cancelled",
            event="push",
            selected_suite=PUSH_SUITE,
            workflow_runs=RUNS_WITH_ANOTHER_BRANCH,
        )
        self.assert_single_failure(failures, f"{API_GATE}: cancelled")

    def test_pull_request_reads_runs_of_every_branch(self) -> None:
        """Outside a push, nothing changes: a pull request reads every suite."""
        failures = run_required_aggregator(
            API_GATE,
            "cancelled",
            event="pull_request",
            selected_suite=OTHER_BRANCH_SUITE,
            workflow_runs=RUNS_WITH_ANOTHER_BRANCH,
        )
        self.assert_single_failure(failures, f"{API_GATE}: cancelled")

    def test_push_ignores_a_skipped_scheduled_run(self) -> None:
        """T-CI-AGGREGATOR-READS-SCHEDULE-RUNS-2026-10-15 (master fdf711ba6, run 38545862225)."""
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
        """Fail when the push's own check skipped even if a scheduled check succeeded."""
        failures = run_required_aggregator(
            SCHEDULED_GATE,
            "skipped",
            event="push",
            selected_suite=PUSH_SUITE,
            workflow_runs=RUNS_WITH_A_SCHEDULE,
            extra_checks=[{**SCHEDULED_SKIP, "conclusion": "success"}],
        )
        self.assert_single_failure(failures, f"{SCHEDULED_GATE}: skipped")


class AggregatorAppCheckWindow(_AggregatorCase):
    """Producer pinning, age window and upload-job propagation rules (ADR-3539)."""

    def test_pull_request_accepts_older_code_scanning_app_checks(self) -> None:
        """Accept older per-SHA runs from github-advanced-security for its three checks."""
        started_at = _minutes_ago(10)
        for name in CODE_SCANNING_CHECKS:
            with self.subTest(name=name):
                old_pass = _check_run(name, started_at=started_at)
                self.assertEqual(_run_pr_checks(name, [old_pass]), [])

    def test_pull_request_fails_older_failed_code_scanning_app_check(self) -> None:
        """Keep failing an older failed check run from github-advanced-security."""
        old_failure = _check_run("CodeQL", "failure", started_at=_minutes_ago(10))
        self.assert_single_failure(_run_pr_checks("CodeQL", [old_failure]), "CodeQL: failure")

    def test_pull_request_ignores_foreign_producer_for_code_scanning_check(self) -> None:
        """Ignore CodeQL check runs posted by any producer other than github-advanced-security."""
        for app in (FOREIGN_APP, ACTIONS_APP):
            for age_minutes in (10, 1):
                with self.subTest(app=app["slug"], age_minutes=age_minutes):
                    spoofed = _check_run("CodeQL", app=app, started_at=_minutes_ago(age_minutes))
                    self.assert_single_failure(
                        _run_pr_checks("CodeQL", [spoofed]), "CodeQL: never reported"
                    )

    def test_foreign_producer_cannot_mask_github_actions_failure(self) -> None:
        """Keep a real GitHub Actions failure when a foreign app posts a newer same-named pass."""
        actions_failure = _check_run(
            "Gitleaks",
            "failure",
            app=ACTIONS_APP,
            suite_id=PUSH_SUITE,
            started_at=_minutes_ago(1),
        )
        foreign_pass = _check_run(
            "Gitleaks",
            "success",
            app=FOREIGN_APP,
            suite_id=CODE_SCANNING_SUITE,
            started_at=_minutes_offset(1),
        )
        failures = _run_pr_checks("Gitleaks", [actions_failure, foreign_pass])
        self.assert_single_failure(failures, "Gitleaks: failure")

    def test_pull_request_ignores_older_github_actions_check_run(self) -> None:
        """Drop GitHub Actions check runs older than minCreatedMs."""
        old_actions_pass = _check_run(
            ACTIONS_CHECK, app=ACTIONS_APP, suite_id=PUSH_SUITE, started_at=_minutes_ago(10)
        )
        self.assert_single_failure(
            _run_pr_checks(ACTIONS_CHECK, [old_actions_pass]), f"{ACTIONS_CHECK}: never reported"
        )

    def test_pull_request_ignores_older_check_run_with_missing_app(self) -> None:
        """Drop unattributed check runs for Actions names when older than minCreatedMs."""
        for app in UNATTRIBUTED_APPS:
            with self.subTest(app=app):
                old_unattributed = _check_run(
                    ACTIONS_CHECK, app=app, suite_id=PUSH_SUITE, started_at=_minutes_ago(10)
                )
                self.assert_single_failure(
                    _run_pr_checks(ACTIONS_CHECK, [old_unattributed]),
                    f"{ACTIONS_CHECK}: never reported",
                )

    def test_pull_request_accepts_in_window_actions_check_with_missing_app(self) -> None:
        """Keep admitting unattributed check runs inside minCreatedMs for Actions names."""
        for app in UNATTRIBUTED_APPS:
            with self.subTest(app=app):
                recent_unattributed = _check_run(
                    ACTIONS_CHECK, app=app, suite_id=PUSH_SUITE, started_at=_minutes_ago(1)
                )
                self.assertEqual(_run_pr_checks(ACTIONS_CHECK, [recent_unattributed]), [])

    def test_pull_request_ignores_missing_app_slug_for_code_scanning_check(self) -> None:
        """Reject unattributed check runs named CodeQL even inside minCreatedMs."""
        for app in UNATTRIBUTED_APPS:
            for age_minutes in (1, 10):
                with self.subTest(app=app, age_minutes=age_minutes):
                    unattributed = _check_run(
                        "CodeQL", app=app, started_at=_minutes_ago(age_minutes)
                    )
                    self.assert_single_failure(
                        _run_pr_checks("CodeQL", [unattributed]), "CodeQL: never reported"
                    )

    def test_newer_in_progress_app_check_beats_older_success_and_waits(self) -> None:
        """Wait when github-advanced-security posts a newer active run over an old pass."""
        old_success = _check_run("CodeQL", started_at=_minutes_ago(10))
        newer_active = _check_run("CodeQL", None, status="in_progress", started_at=_minutes_ago(5))
        self.assert_single_failure(
            _run_pr_checks("CodeQL", [old_success, newer_active]), "CodeQL: in_progress"
        )

        newer_done = {**newer_active, "status": "completed", "conclusion": "success"}
        completed = _run_pr_checks(
            "CodeQL",
            [old_success, newer_active],
            later_checks=[*_other_required_checks_succeed(("CodeQL",)), old_success, newer_done],
        )
        self.assertEqual(completed, [])

    def test_active_upload_job_waits_despite_older_app_success(self) -> None:
        """Keep waiting while CodeQL (C/C++) runs even when an older CodeQL pass exists."""
        old_codeql = _check_run("CodeQL", started_at=_minutes_ago(10))
        active_upload = _check_run(
            "CodeQL (C/C++)",
            None,
            app=ACTIONS_APP,
            suite_id=PUSH_SUITE,
            status="in_progress",
            started_at=_minutes_ago(1),
        )
        uncompleted = _run_pr_checks(
            "CodeQL",
            [old_codeql, active_upload],
            absent_checks=("CodeQL (C/C++)",),
        )
        self.assert_single_failure(uncompleted, "CodeQL (C/C++): in_progress")

        finished_upload = {**active_upload, "status": "completed", "conclusion": "success"}
        newer_failure = _check_run("CodeQL", "failure", started_at=_minutes_offset(1))
        later = [
            *_other_required_checks_succeed(("CodeQL", "CodeQL (C/C++)")),
            old_codeql,
            finished_upload,
            newer_failure,
        ]
        completed = _run_pr_checks(
            "CodeQL",
            [old_codeql, active_upload],
            absent_checks=("CodeQL (C/C++)",),
            later_checks=later,
        )
        self.assert_single_failure(completed, "CodeQL: failure")

    def test_propagating_upload_job_waits_for_external_result_despite_old_success(self) -> None:
        """Keep polling through registrationPropagationMs after each SARIF upload job completes."""
        for external_name, upload_job in UPLOAD_JOB_PAIRS:
            with self.subTest(external_name=external_name, upload_job=upload_job):
                old_pass = _check_run(external_name, started_at=_minutes_ago(10))
                just_finished_upload = _check_run(
                    upload_job,
                    "success",
                    app=ACTIONS_APP,
                    suite_id=PUSH_SUITE,
                    started_at=_minutes_ago(1),
                    completed_at=_minutes_offset(WITHIN_FIRST_POLL_PROPAGATION_MIN),
                )
                newer_failure = _check_run(external_name, "failure", started_at=_minutes_offset(1))
                later = [
                    *_other_required_checks_succeed((external_name, upload_job)),
                    old_pass,
                    just_finished_upload,
                    newer_failure,
                ]
                failures = _run_pr_checks(
                    external_name,
                    [old_pass, just_finished_upload],
                    absent_checks=(upload_job,),
                    later_checks=later,
                )
                self.assert_single_failure(failures, f"{external_name}: failure")

    def test_upload_job_completed_beyond_propagation_window_accepts_old_app_success(self) -> None:
        """Stop waiting once the upload job completed further back than the propagation window."""
        old_codeql = _check_run("CodeQL", started_at=_minutes_ago(10))
        settled_upload = _check_run(
            "CodeQL (C/C++)",
            "success",
            app=ACTIONS_APP,
            suite_id=PUSH_SUITE,
            started_at=_minutes_ago(1),
            completed_at=_minutes_offset(PAST_FIRST_POLL_PROPAGATION_MIN),
        )
        unreached_later = [
            *_other_required_checks_succeed(("CodeQL", "CodeQL (C/C++)")),
            settled_upload,
            _check_run("CodeQL", "failure", started_at=_minutes_offset(1)),
        ]
        failures = _run_pr_checks(
            "CodeQL",
            [old_codeql, settled_upload],
            absent_checks=("CodeQL (C/C++)",),
            later_checks=unreached_later,
        )
        self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
