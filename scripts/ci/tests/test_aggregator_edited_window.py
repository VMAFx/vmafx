#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Required Checks Aggregator edited-event window and anchor tests (ADR-3551, #3459).

On pull_request `edited` events, only rule-enforcement gates re-run while earlier
ready-head build and test checks stay at their original timestamps. The aggregator
anchors `minCreatedMs` at `anchorMs - 2 * 60 * 1000` (`runTimestampMs >= minCreatedMs`
inclusive; `< minCreatedMs` excluded), where `anchorMs` is the latest `ready_for_review`
timeline event or `pull_request.created_at` when never a draft. Non-edited events keep
the aggregator run's own `created_at`.

AGGREGATOR_UNDER_TEST points the suite at another workflow copy for mutation proofs.
"""

from __future__ import annotations

import contextlib
import os
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import required_aggregator_harness as harness
from scripts.ci.required_aggregator_harness import run_required_aggregator

RUN_CREATED_AT = "2026-11-15T05:00:00.000Z"
MIN_30_AGO = -30 * 60
MIN_60_AGO = -60 * 60
MIN_90_AGO = -90 * 60
MIN_100_AGO = -100 * 60
MIN_120_AGO = -120 * 60
AFTER_EDIT_S = 10
BODY_GATE = "Deliverables Checklist"
TITLE_GATE = "DCO Sign-off"
BUILD_CHECK = "Pre-Commit"
RERUN_SUITE = 2

_OVERRIDE = contextlib.ExitStack()


def setUpModule() -> None:
    """Point the harness at AGGREGATOR_UNDER_TEST when set."""
    override = os.environ.get("AGGREGATOR_UNDER_TEST")
    if override:
        _OVERRIDE.enter_context(mock.patch.object(harness, "AGGREGATOR_PATH", Path(override)))


def tearDownModule() -> None:
    """Restore any patched aggregator path."""
    _OVERRIDE.close()


class AggregatorEditedTriggerContract(unittest.TestCase):
    """Workflow trigger contract for pull_request edited events."""

    def test_workflow_pull_request_trigger_includes_edited(self) -> None:
        """Require pull_request.types to list edited in canonical order."""
        text = harness.AGGREGATOR_PATH.read_text(encoding="utf-8")
        match = re.search(
            r"(?m)^\s*pull_request:\s*\n(?:\s+.*\n)*?\s+types:\s*\[([^\]]+)\]",
            text,
        )
        self.assertIsNotNone(match, "required-aggregator.yml must declare pull_request.types")
        types = [item.strip() for item in (match.group(1) if match else "").split(",")]
        self.assertEqual(
            types,
            ["opened", "edited", "synchronize", "reopened", "ready_for_review"],
        )


class AggregatorEditedWindowTests(unittest.TestCase):
    """Edited-event window anchor, boundary, draft-era exclusion and fail-closed checks."""

    def _assert_single_failed_check(self, failures: list[str], expected: str) -> None:
        """Assert that only one required check failed with ``expected`` in its line."""
        self.assertEqual(len(failures), 1, failures)
        lines = failures[0].splitlines()
        self.assertEqual(len(lines), 2, failures[0])
        self.assertEqual(lines[0], "Required checks did not pass:")
        self.assertIn(expected, lines[1])

    def test_edited_after_green_body_gate_rerun_failure_blocks(self) -> None:
        """Fail naming Deliverables Checklist when a body edit turns that re-run gate red."""
        rerun_failure = {
            "name": BODY_GATE,
            "conclusion": "failure",
            "check_suite": {"id": RERUN_SUITE},
            "started_at_offset_s": AFTER_EDIT_S,
        }
        failures = run_required_aggregator(
            BODY_GATE,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=[{"event": "ready_for_review", "created_at_offset_s": MIN_60_AGO}],
            default_started_offset_s=MIN_30_AGO,
            extra_checks=[rerun_failure],
        )
        self._assert_single_failed_check(failures, f"{BODY_GATE}: failure")

    def test_edited_after_green_title_gate_rerun_failure_blocks(self) -> None:
        """Fail naming DCO Sign-off when a title edit turns that re-run gate red."""
        rerun_failure = {
            "name": TITLE_GATE,
            "conclusion": "failure",
            "check_suite": {"id": RERUN_SUITE},
            "started_at_offset_s": AFTER_EDIT_S,
        }
        failures = run_required_aggregator(
            TITLE_GATE,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=[{"event": "ready_for_review", "created_at_offset_s": MIN_60_AGO}],
            default_started_offset_s=MIN_30_AGO,
            extra_checks=[rerun_failure],
        )
        self._assert_single_failed_check(failures, f"{TITLE_GATE}: failure")

    def test_edited_all_green_counts_prior_ready_checks(self) -> None:
        """Pass when re-run gates succeed and build checks succeeded 30 min earlier."""
        reruns = [
            {
                "name": gate,
                "conclusion": "success",
                "check_suite": {"id": RERUN_SUITE},
                "started_at_offset_s": AFTER_EDIT_S,
            }
            for gate in (BODY_GATE, TITLE_GATE)
        ]
        infos: list[str] = []
        failures = run_required_aggregator(
            BODY_GATE,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=[{"event": "ready_for_review", "created_at_offset_s": MIN_60_AGO}],
            default_started_offset_s=MIN_30_AGO,
            extra_checks=reruns,
            info_logs=infos,
        )
        self.assertEqual(failures, [])
        self.assertTrue(
            any(msg.startswith("window anchor: ready_for_review at ") for msg in infos),
            infos,
        )

    def test_edited_never_draft_anchors_at_pr_created_at(self) -> None:
        """Use pull_request.created_at when the timeline has no ready_for_review event."""
        infos: list[str] = []
        failures = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            pr_created_offset_s=MIN_90_AGO,
            timeline_events=[],
            default_started_offset_s=MIN_30_AGO,
            info_logs=infos,
        )
        self.assertEqual(failures, [])
        self.assertTrue(
            any(msg.startswith("window anchor: pull request created at ") for msg in infos),
            infos,
        )

    def test_edited_excludes_draft_era_skipped_check_before_ready_window(self) -> None:
        """Exclude a draft-era skipped check created before ready_for_review minus 2 min."""
        draft_skipped = {
            "name": BUILD_CHECK,
            "conclusion": "skipped",
            "check_suite": {"id": 1},
            "created_at_offset_s": MIN_60_AGO - 5 * 60,
        }
        failures = run_required_aggregator(
            BUILD_CHECK,
            None,
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=[{"event": "ready_for_review", "created_at_offset_s": MIN_60_AGO}],
            default_started_offset_s=MIN_30_AGO,
            extra_checks=[draft_skipped],
        )
        self._assert_single_failed_check(failures, f"{BUILD_CHECK}: never reported")

    def test_boundary_at_anchor_minus_two_minutes_is_inclusive(self) -> None:
        """newestByName drops `< minCreatedMs`: anchor - 2m counts; anchor - 2m - 1s fails."""
        ready_timeline = [{"event": "ready_for_review", "created_at_offset_s": MIN_60_AGO}]
        exact_edge = MIN_60_AGO - 120
        before_edge = exact_edge - 1

        at_boundary = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=ready_timeline,
            default_started_offset_s=MIN_30_AGO,
            created_offsets_s={BUILD_CHECK: exact_edge},
        )
        self.assertEqual(at_boundary, [])

        past_boundary = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=ready_timeline,
            default_started_offset_s=MIN_30_AGO,
            created_offsets_s={BUILD_CHECK: before_edge},
        )
        self._assert_single_failed_check(past_boundary, f"{BUILD_CHECK}: never reported")

    def test_non_edited_synchronize_still_ignores_older_checks(self) -> None:
        """Keep own-run anchor on synchronize even when timeline has ready_for_review."""
        infos: list[str] = []
        failures = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="synchronize",
            run_created_at=RUN_CREATED_AT,
            pr_created_offset_s=MIN_90_AGO,
            timeline_events=[{"event": "ready_for_review", "created_at_offset_s": MIN_60_AGO}],
            created_offsets_s={BUILD_CHECK: MIN_30_AGO},
            info_logs=infos,
        )
        self._assert_single_failed_check(failures, f"{BUILD_CHECK}: never reported")
        self.assertTrue(
            any(msg.startswith("window anchor: own run created at ") for msg in infos),
            infos,
        )

    def test_timeline_api_always_500_fails_closed_without_judging_checks(self) -> None:
        """Report Transient GitHub API failure and judge no check when timeline stays 500."""
        failures = run_required_aggregator(
            BUILD_CHECK,
            "failure",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            api_always={"listEventsForTimeline": 500},
        )
        self.assertEqual(len(failures), 1, failures)
        self.assertTrue(
            failures[0].startswith(
                "Transient GitHub API failure: listEventsForTimeline still answers HTTP 500"
            ),
            failures,
        )
        self.assertIn("No check result was judged", failures[0])
        self.assertNotIn(BUILD_CHECK, failures[0])

    def test_multiple_ready_for_review_events_anchors_at_latest(self) -> None:
        """Use the latest ready_for_review across draft -> ready -> draft -> ready."""
        timeline = [
            {"event": "ready_for_review", "created_at_offset_s": MIN_120_AGO},
            {"event": "convert_to_draft", "created_at_offset_s": MIN_90_AGO},
            {"event": "ready_for_review", "created_at_offset_s": MIN_60_AGO},
        ]
        stale_between_transitions = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=timeline,
            default_started_offset_s=MIN_30_AGO,
            started_offsets_s={BUILD_CHECK: MIN_100_AGO},
        )
        self._assert_single_failed_check(
            stale_between_transitions, f"{BUILD_CHECK}: never reported"
        )

        fresh_after_latest = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=timeline,
            default_started_offset_s=MIN_30_AGO,
        )
        self.assertEqual(fresh_after_latest, [])

    def test_timeline_transient_5xx_retries_and_succeeds(self) -> None:
        """Retry transient 5xx responses on listEventsForTimeline and pass once healthy."""
        failures = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=[{"event": "ready_for_review", "created_at_offset_s": MIN_60_AGO}],
            default_started_offset_s=MIN_30_AGO,
            api_errors={"listEventsForTimeline": [500, 503]},
        )
        self.assertEqual(failures, [])

    def test_timeline_pagination_and_missing_anchor_fail_closed(self) -> None:
        """Walk paginated timeline pages and fail closed when anchor cannot be derived."""
        paginated = [{"event": "ready_for_review", "created_at_offset_s": MIN_120_AGO}]
        paginated.extend({"event": "commented"} for _ in range(110))
        paginated.append({"event": "ready_for_review", "created_at_offset_s": MIN_60_AGO})
        stale_on_page_two = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=paginated,
            default_started_offset_s=MIN_30_AGO,
            started_offsets_s={BUILD_CHECK: MIN_100_AGO},
        )
        self._assert_single_failed_check(stale_on_page_two, f"{BUILD_CHECK}: never reported")

        missing_anchor = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=[],
            default_started_offset_s=MIN_30_AGO,
        )
        self.assertEqual(len(missing_anchor), 1, missing_anchor)
        self.assertIn("Cannot derive window anchor", missing_anchor[0])

        overflow_pages = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            pr_created_offset_s=MIN_90_AGO,
            timeline_events=[{"event": "commented"} for _ in range(1000)],
            default_started_offset_s=MIN_30_AGO,
        )
        self.assertEqual(len(overflow_pages), 1, overflow_pages)
        self.assertIn("Cannot derive window anchor", overflow_pages[0])


if __name__ == "__main__":
    unittest.main()
