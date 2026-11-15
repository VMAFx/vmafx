#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Required Checks Aggregator edited-event window and anchor tests (ADR-3551, #3459).

On pull_request `edited` events, only rule-enforcement gates re-run while earlier
ready-head build and test checks stay at their original timestamps. The aggregator
anchors `minCreatedMs` at `anchorMs - 2 * 60 * 1000` (`runTimestampMs >= minCreatedMs`
inclusive; `< minCreatedMs` excluded), where `anchorMs` is the latest `ready_for_review`
timeline event or `pull_request.created_at` when never a draft. Workflows listed in
`editedRerunWorkflows` must also report from a current-edit run whose `display_title`
starts with `[<pull_request.updated_at>] `; every other run of those workflows is stale
and its check suite is excluded, failing closed when `pull_request.updated_at` is missing
or no current run starts. Non-edited events keep the aggregator run's own `created_at`.

AGGREGATOR_UNDER_TEST and REPO_ROOT_UNDER_TEST point the suite at scratch copies for
mutation proofs.
"""

from __future__ import annotations

import contextlib
import os
import re
import sys
import unittest
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any
from unittest import mock

import yaml  # type: ignore[import-untyped]

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
THIRTY_SEC_AGO = -30
STAMP_T1 = "2026-11-15T04:59:30Z"
STAMP_T2_MINUS_1S = "2026-11-15T04:59:59Z"
STAMP_T2 = harness.DEFAULT_EDIT_STAMP
PR_TITLE = "fix(ci): demo pull request"
EDIT_STAMP_RUN_NAME_PREFIX = "[${{ github.event.pull_request.updated_at }}] "
BODY_GATE = "Deliverables Checklist"
TITLE_GATE = "DCO Sign-off"
BUILD_CHECK = "Pre-Commit"
BUILD_SUITE = 1
RERUN_SUITE = 2
OLD_RULES_SUITE = 2
CURRENT_RULES_SUITE = 3
RULES_WORKFLOW = ".github/workflows/rule-enforcement.yml"
BUILD_WORKFLOW = ".github/workflows/lint-and-format.yml"
PR_TYPE_LABEL_WORKFLOW = ".github/workflows/pr-type-label.yml"
PULL_REQUEST_DEFAULT_TYPES = frozenset({"opened", "synchronize", "reopened"})
RULES_GATES = (
    "Deliverables Checklist",
    "Doc-Substance Gate",
    "docs/state.md Gate",
    "Silent-Revert Guard",
    "DCO Sign-off",
    "FFmpeg-Patches Surface Sync",
    "ADR Collision Guard",
    "Release Script Contract",
)
RULES_SUITE_MAP = dict.fromkeys(RULES_GATES, OLD_RULES_SUITE)
READY_TIMELINE = ({"event": "ready_for_review", "created_at_offset_s": MIN_60_AGO},)

_OVERRIDE = contextlib.ExitStack()


def setUpModule() -> None:
    """Point the harness at REPO_ROOT_UNDER_TEST or AGGREGATOR_UNDER_TEST when set."""
    repo_override = os.environ.get("REPO_ROOT_UNDER_TEST")
    if repo_override:
        root = Path(repo_override)
        _OVERRIDE.enter_context(mock.patch.object(harness, "REPO_ROOT", root))
        _OVERRIDE.enter_context(
            mock.patch.object(
                harness,
                "AGGREGATOR_PATH",
                root / ".github" / "workflows" / "required-aggregator.yml",
            )
        )
    override = os.environ.get("AGGREGATOR_UNDER_TEST")
    if override:
        _OVERRIDE.enter_context(mock.patch.object(harness, "AGGREGATOR_PATH", Path(override)))


def tearDownModule() -> None:
    """Restore any patched aggregator path."""
    _OVERRIDE.close()


def _rules_run(
    suite_id: int,
    created_at_offset_s: float = 0,
    *,
    display_title: str | None = None,
    edit_stamp: str | None = None,
) -> dict[str, object]:
    """Build one synthetic rule-enforcement.yml workflow-run entry."""
    resolved_title = (
        display_title
        if display_title is not None
        else (f"[{edit_stamp}] {PR_TITLE}" if edit_stamp is not None else None)
    )
    run: dict[str, object] = {
        "path": RULES_WORKFLOW,
        "event": "pull_request",
        "check_suite_id": suite_id,
        "created_at_offset_s": created_at_offset_s,
    }
    if resolved_title is not None:
        run["display_title"] = resolved_title
    return run


def _rules_suite_checks(
    suite_id: int,
    *,
    conclusions: Mapping[str, str] | None = None,
    started_offset_s: float = AFTER_EDIT_S,
) -> list[dict[str, object]]:
    """Build synthetic check runs for all required rule-enforcement.yml gates."""
    overrides = conclusions or {}
    return [
        {
            "name": gate,
            "conclusion": overrides.get(gate, "success"),
            "check_suite": {"id": suite_id},
            "started_at_offset_s": started_offset_s,
        }
        for gate in RULES_GATES
    ]


def _workflow_pull_request_types(workflow: dict[Any, Any]) -> set[str]:
    raw_on = workflow.get(True, workflow.get("on"))
    if isinstance(raw_on, str):
        triggers: dict[str, Any] = {raw_on: None}
    elif isinstance(raw_on, list):
        triggers = dict.fromkeys(raw_on)
    else:
        triggers = dict(raw_on or {})
    if "pull_request" not in triggers:
        return set()
    pr_cfg = triggers["pull_request"]
    types = pr_cfg.get("types") if isinstance(pr_cfg, dict) else None
    if types is None:
        return set(PULL_REQUEST_DEFAULT_TYPES)
    return {str(item) for item in types}


def _expand_include_rows(template: str, rows: list[Any]) -> set[str]:
    names: set[str] = set()
    for row in rows:
        if not isinstance(row, dict):
            continue
        if isinstance(row.get("name"), str):
            names.add(row["name"])
        for key, val in row.items():
            names.add(template.replace(f"${{{{ matrix.{key} }}}}", str(val)))
    return names


def _expand_matrix_names(template: str, matrix: dict[str, Any]) -> set[str]:
    names = {template}
    for key, values in matrix.items():
        if not isinstance(values, list):
            continue
        if key == "include":
            names.update(_expand_include_rows(template, values))
        else:
            names.update(template.replace(f"${{{{ matrix.{key} }}}}", str(v)) for v in values)
    return names


def _workflow_required_jobs(workflow: dict[Any, Any], required: set[str]) -> set[str]:
    matched: set[str] = set()
    for job_id, job_body in (workflow.get("jobs") or {}).items():
        if not isinstance(job_body, dict):
            continue
        template = str(job_body.get("name", job_id))
        strategy = job_body.get("strategy")
        matrix = strategy.get("matrix") if isinstance(strategy, dict) else None
        candidates = (
            _expand_matrix_names(template, matrix) if isinstance(matrix, dict) else {template}
        )
        matched.update(candidates & required)
    return matched


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

    def test_edited_rerun_workflows_stay_in_sync_with_workflow_triggers(self) -> None:
        """Keep editedRerunWorkflows synced with workflows that rerun required jobs on edited."""
        script = harness._embedded_script(harness.AGGREGATOR_PATH.read_text(encoding="utf-8"))
        required = set(harness._required_names(script))
        declared_list = harness._edited_rerun_workflows(script)
        declared = set(declared_list)
        self.assertEqual(len(declared_list), len(declared), "duplicate editedRerunWorkflows entry")

        workflows_dir = harness.REPO_ROOT / ".github" / "workflows"
        files = sorted(workflows_dir.glob("*.yml")) + sorted(workflows_dir.glob("*.yaml"))
        expected_rerun: set[str] = set()
        edited_advisory: set[str] = set()
        for path in files:
            if path.name == "required-aggregator.yml":
                continue
            parsed = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            if "edited" not in _workflow_pull_request_types(parsed):
                continue
            rel_path = f".github/workflows/{path.name}"
            if _workflow_required_jobs(parsed, required):
                expected_rerun.add(rel_path)
            else:
                # pr-type-label.yml triggers on `edited` to refresh `type:*` labels from PR
                # titles (`Label PR by commit type`), which is advisory and not in `required`,
                # so it produces no required check to exclude or wait on.
                edited_advisory.add(rel_path)

        self.assertIn(PR_TYPE_LABEL_WORKFLOW, edited_advisory)
        self.assertEqual(declared, expected_rerun)
        for rel_path in declared_list:
            workflow_file = harness.REPO_ROOT / rel_path
            self.assertTrue(workflow_file.is_file(), rel_path)
            parsed = yaml.safe_load(workflow_file.read_text(encoding="utf-8")) or {}
            self.assertIn("edited", _workflow_pull_request_types(parsed), rel_path)
            run_name = parsed.get("run-name")
            self.assertIsInstance(run_name, str, f"{rel_path} must declare top-level run-name")
            self.assertTrue(
                isinstance(run_name, str) and run_name.startswith(EDIT_STAMP_RUN_NAME_PREFIX),
                f"{rel_path} run-name must start with {EDIT_STAMP_RUN_NAME_PREFIX!r}",
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

    def _run_edited_suites(
        self,
        workflow_runs: Sequence[Mapping[str, object]],
        *,
        pr_updated_at: str | None = STAMP_T2,
        extra_checks: Sequence[Mapping[str, object]] = (),
        started_offsets_s: Mapping[str, float] | None = None,
        later_workflow_runs: Sequence[Mapping[str, object]] = (),
        info_logs: list[str] | None = None,
    ) -> list[str]:
        """Run edited-event aggregator with baseline Rules checks in OLD_RULES_SUITE."""
        return run_required_aggregator(
            BODY_GATE,
            "success",
            action="edited",
            pr_updated_at=pr_updated_at,
            run_created_at=RUN_CREATED_AT,
            timeline_events=READY_TIMELINE,
            default_started_offset_s=MIN_30_AGO,
            started_offsets_s=started_offsets_s,
            suite_by_check=RULES_SUITE_MAP,
            workflow_runs=workflow_runs,
            later_workflow_runs=later_workflow_runs,
            extra_checks=extra_checks,
            info_logs=info_logs,
        )

    def test_two_edits_30s_apart_earlier_green_rules_run_blocks(self) -> None:
        """Reject a green [T1] Rules run created 30 s before a [T2] edited aggregator."""
        infos: list[str] = []
        failures = self._run_edited_suites(
            [_rules_run(OLD_RULES_SUITE, THIRTY_SEC_AGO, edit_stamp=STAMP_T1)],
            started_offsets_s=dict.fromkeys(RULES_GATES, THIRTY_SEC_AGO + 5),
            info_logs=infos,
        )
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("rule-enforcement.yml", failures[0])
        self.assertIn("the Rules workflow did not start for this edit", failures[0])
        self.assertIn("Excluded 1 stale edited-rerun check suite(s)", infos)

    def test_edited_race_old_rules_suite_without_current_run_blocks(self) -> None:
        """Fail naming rule-enforcement.yml when only a stale Rules suite exists on edited."""
        infos: list[str] = []
        no_current_run = self._run_edited_suites(
            [_rules_run(OLD_RULES_SUITE, MIN_30_AGO, edit_stamp=STAMP_T1)],
            info_logs=infos,
        )
        self.assertEqual(len(no_current_run), 1, no_current_run)
        self.assertIn("rule-enforcement.yml", no_current_run[0])
        self.assertIn("the Rules workflow did not start for this edit", no_current_run[0])
        self.assertIn("Excluded 1 stale edited-rerun check suite(s)", infos)

        unregistered_gates = self._run_edited_suites(
            [
                _rules_run(OLD_RULES_SUITE, MIN_30_AGO, edit_stamp=STAMP_T1),
                _rules_run(CURRENT_RULES_SUITE, 0, edit_stamp=STAMP_T2),
            ],
        )
        self.assertEqual(len(unregistered_gates), 1, unregistered_gates)
        self.assertIn(f"{BODY_GATE}: never reported", unregistered_gates[0])

    def test_edited_current_rules_gate_failure_not_masked_by_stale_suite(self) -> None:
        """Fail on [T2] Deliverables Checklist or DCO Sign-off despite newer green [T1]."""
        for failing_gate in (BODY_GATE, TITLE_GATE):
            with self.subTest(failing_gate=failing_gate):
                infos: list[str] = []
                failures = self._run_edited_suites(
                    [
                        _rules_run(OLD_RULES_SUITE, THIRTY_SEC_AGO, edit_stamp=STAMP_T1),
                        _rules_run(CURRENT_RULES_SUITE, 0, edit_stamp=STAMP_T2),
                    ],
                    started_offsets_s={failing_gate: AFTER_EDIT_S + 30},
                    extra_checks=_rules_suite_checks(
                        CURRENT_RULES_SUITE, conclusions={failing_gate: "failure"}
                    ),
                    info_logs=infos,
                )
                self._assert_single_failed_check(failures, f"{failing_gate}: failure")
                self.assertIn("Excluded 1 stale edited-rerun check suite(s)", infos)

    def test_edited_current_rules_all_green_counts_prior_build_checks(self) -> None:
        """Pass when [T2] Rules suite is green and build checks ran 30 min before edit."""
        only_current_infos: list[str] = []
        only_current = self._run_edited_suites(
            [_rules_run(OLD_RULES_SUITE, 0, edit_stamp=STAMP_T2)],
            info_logs=only_current_infos,
        )
        self.assertEqual(only_current, [])
        self.assertIn("Excluded 0 stale edited-rerun check suite(s)", only_current_infos)

        infos: list[str] = []
        failures = run_required_aggregator(
            BODY_GATE,
            "failure",
            action="edited",
            pr_updated_at=STAMP_T2,
            run_created_at=RUN_CREATED_AT,
            timeline_events=READY_TIMELINE,
            default_started_offset_s=MIN_30_AGO,
            suite_by_check=RULES_SUITE_MAP,
            workflow_runs=[
                _rules_run(OLD_RULES_SUITE, MIN_30_AGO, edit_stamp=STAMP_T1),
                _rules_run(CURRENT_RULES_SUITE, 0, edit_stamp=STAMP_T2),
            ],
            extra_checks=_rules_suite_checks(CURRENT_RULES_SUITE),
            info_logs=infos,
        )
        self.assertEqual(failures, [])
        self.assertIn("Excluded 1 stale edited-rerun check suite(s)", infos)

    def test_edit_stamp_boundary_exact_current_and_off_by_1s_or_missing_stale(self) -> None:
        """Exact [T2] prefix is CURRENT; [T2 - 1 s], missing title, or no space is STALE."""
        exact_infos: list[str] = []
        exact = self._run_edited_suites(
            [_rules_run(OLD_RULES_SUITE, MIN_30_AGO, edit_stamp=STAMP_T2)],
            info_logs=exact_infos,
        )
        self.assertEqual(exact, [])
        self.assertIn("Excluded 0 stale edited-rerun check suite(s)", exact_infos)

        for label, stale_run in (
            ("one_second_off", _rules_run(OLD_RULES_SUITE, 0, edit_stamp=STAMP_T2_MINUS_1S)),
            ("missing_display_title", _rules_run(OLD_RULES_SUITE, 0)),
            (
                "missing_trailing_space",
                _rules_run(OLD_RULES_SUITE, 0, display_title=f"[{STAMP_T2}]"),
            ),
        ):
            with self.subTest(case=label):
                failures = self._run_edited_suites([stale_run])
                self.assertEqual(len(failures), 1, failures)
                self.assertIn("rule-enforcement.yml", failures[0])

    def test_edit_stamp_boundary_stamp_not_at_start_is_stale(self) -> None:
        """Treat display_title with [T2] outside position 0 (`title [T2]`) as STALE."""
        for bad_title in (f"title [{STAMP_T2}]", f"title [{STAMP_T2}] trailing"):
            with self.subTest(display_title=bad_title):
                failures = self._run_edited_suites(
                    [_rules_run(OLD_RULES_SUITE, 0, display_title=bad_title)]
                )
                self.assertEqual(len(failures), 1, failures)
                self.assertIn("rule-enforcement.yml", failures[0])

    def test_edit_stamp_boundary_prefix_t1_with_t2_in_title_is_stale(self) -> None:
        """Treat `[T1] [T2] title` (author title starts with `[T2] `) as STALE on edit T2."""
        crafted = f"[{STAMP_T1}] [{STAMP_T2}] title"
        failures = self._run_edited_suites([_rules_run(OLD_RULES_SUITE, 0, display_title=crafted)])
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("rule-enforcement.yml", failures[0])

    def test_edited_missing_own_updated_at_fails_closed(self) -> None:
        """Fail closed naming pull_request.updated_at when the edited payload lacks a stamp."""
        for bad_stamp in (None, "", "   "):
            with self.subTest(pr_updated_at=bad_stamp):
                failures = self._run_edited_suites(
                    [_rules_run(OLD_RULES_SUITE, 0, edit_stamp=STAMP_T1)],
                    pr_updated_at=bad_stamp,
                )
                self.assertEqual(len(failures), 1, failures)
                self.assertIn("pull_request.updated_at", failures[0])

    def test_non_rules_suite_keeps_r2_ready_anchor(self) -> None:
        """Keep ready_for_review anchor for build checks in a non-Rules suite 30 min old."""
        build_run = {
            "path": BUILD_WORKFLOW,
            "event": "pull_request",
            "check_suite_id": BUILD_SUITE,
            "created_at_offset_s": MIN_30_AGO,
        }
        failures = run_required_aggregator(
            BUILD_CHECK,
            "success",
            action="edited",
            pr_updated_at=STAMP_T2,
            run_created_at=RUN_CREATED_AT,
            timeline_events=READY_TIMELINE,
            default_started_offset_s=MIN_30_AGO,
            suite_by_check=RULES_SUITE_MAP,
            workflow_runs=[build_run, _rules_run(CURRENT_RULES_SUITE, 0, edit_stamp=STAMP_T2)],
            extra_checks=_rules_suite_checks(CURRENT_RULES_SUITE),
        )
        self.assertEqual(failures, [])

    def test_workflow_runs_api_always_500_on_edited_fails_closed(self) -> None:
        """Report Transient GitHub API failure and judge no check when workflow_runs stays 500."""
        failures = run_required_aggregator(
            BUILD_CHECK,
            "failure",
            action="edited",
            run_created_at=RUN_CREATED_AT,
            timeline_events=READY_TIMELINE,
            api_always={"listWorkflowRunsForRepo": 500},
        )
        self.assertEqual(len(failures), 1, failures)
        self.assertTrue(
            failures[0].startswith(
                "Transient GitHub API failure: listWorkflowRunsForRepo still answers HTTP 500"
            ),
            failures,
        )
        self.assertIn("No check result was judged", failures[0])
        self.assertNotIn(BUILD_CHECK, failures[0])

    def test_synchronize_ignores_edited_stale_suite_exclusion(self) -> None:
        """Skip stale-suite exclusion and workflow-runs calls on pull_request synchronize."""
        infos: list[str] = []
        failures = run_required_aggregator(
            BODY_GATE,
            "success",
            action="synchronize",
            run_created_at=RUN_CREATED_AT,
            timeline_events=READY_TIMELINE,
            suite_by_check=RULES_SUITE_MAP,
            workflow_runs=[_rules_run(OLD_RULES_SUITE, MIN_30_AGO)],
            api_always={"listWorkflowRunsForRepo": 500},
            info_logs=infos,
        )
        self.assertEqual(failures, [])
        self.assertFalse(any("stale edited-rerun" in msg for msg in infos), infos)

    def test_edited_relists_workflow_runs_before_grace_expires(self) -> None:
        """Keep polling within registration grace until the current Rules run appears."""
        infos: list[str] = []
        failures = self._run_edited_suites(
            [_rules_run(OLD_RULES_SUITE, MIN_30_AGO, edit_stamp=STAMP_T1)],
            later_workflow_runs=[
                _rules_run(OLD_RULES_SUITE, MIN_30_AGO, edit_stamp=STAMP_T1),
                _rules_run(CURRENT_RULES_SUITE, 5, edit_stamp=STAMP_T2),
            ],
            extra_checks=_rules_suite_checks(CURRENT_RULES_SUITE),
            info_logs=infos,
        )
        self.assertEqual(failures, [])
        self.assertIn(f"Waiting for edited-event workflow run: {RULES_WORKFLOW}", infos)
        self.assertIn("Excluded 1 stale edited-rerun check suite(s)", infos)


if __name__ == "__main__":
    unittest.main()
