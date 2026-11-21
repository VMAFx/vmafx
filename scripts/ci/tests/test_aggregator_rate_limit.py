#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise rate-limit recovery and adaptive polling against the real script."""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import required_aggregator_harness as harness
from scripts.ci.required_aggregator_harness import SimulationControls, run_required_aggregator
from scripts.lib.safe_subprocess import CommandFailed

ROOT = Path(__file__).resolve().parents[3]
CONFIG = json.loads((ROOT / ".github/ci-tier.json").read_text(encoding="utf-8"))
ENV = {
    "CI_TIER": "light",
    "CI_TIER_ALWAYS": json.dumps(CONFIG["always"]),
    "CI_TIER_FULL_ONLY": json.dumps(CONFIG["full_only"]),
}
CHECK = "Pre-Commit"
START = "2026-11-20T00:00:00Z"
MIN_WAIT_MS = 60000
MAX_WAIT_MS = 3605000
EPOCH = int(datetime.fromisoformat(START.replace("Z", "+00:00")).timestamp())


def detail(seconds: int) -> dict[str, dict[str, object]]:
    return {
        "listForRef": {
            "headers": {
                "x-ratelimit-remaining": "0",
                "x-ratelimit-reset": str(EPOCH + seconds),
            }
        }
    }


def pending(status: str) -> list[dict[str, object]]:
    script = harness._embedded_script(harness.AGGREGATOR_PATH.read_text(encoding="utf-8"))
    return [
        {
            "name": name,
            "status": status if name == CHECK else "completed",
            "conclusion": None if name == CHECK else "success",
            "check_suite": {"id": 1},
        }
        for name in harness._required_names(script)
    ]


class DriverOutput(unittest.TestCase):
    def test_integer_sleeps_extend_list(self) -> None:
        sleeps: list[int] = []
        self.assertEqual(
            harness._decode_driver_output('{"failures": [], "sleeps": [1, 2]}', None, sleeps),
            [],
        )
        self.assertEqual(sleeps, [1, 2])

    def test_boolean_sleep_rejected(self) -> None:
        with self.assertRaisesRegex(AssertionError, "aggregator sleeps must be integers"):
            harness._decode_driver_output('{"failures": [], "sleeps": [true]}', None, [])

    def test_string_sleep_rejected(self) -> None:
        with self.assertRaisesRegex(AssertionError, "aggregator sleeps must be integers"):
            harness._decode_driver_output('{"failures": [], "sleeps": ["1"]}', None, [])

    def test_float_sleep_rejected(self) -> None:
        with self.assertRaisesRegex(AssertionError, "aggregator sleeps must be integers"):
            harness._decode_driver_output('{"failures": [], "sleeps": [1.5]}', None, [])

    def test_empty_sleeps_add_nothing(self) -> None:
        sleeps = [3]
        self.assertEqual(
            harness._decode_driver_output('{"failures": [], "sleeps": []}', None, sleeps),
            [],
        )
        self.assertEqual(sleeps, [3])

    def test_none_sleeps_ignores_payload(self) -> None:
        self.assertEqual(
            harness._decode_driver_output('{"failures": [], "sleeps": [true]}', None, None),
            [],
        )


class RateLimit(unittest.TestCase):
    def primary(self, sleeps: list[int]) -> list[str]:
        return run_required_aggregator(
            CHECK,
            "success",
            env=ENV,
            run_created_at=START,
            api_errors={"listForRef": [403]},
            api_error_detail=detail(3600),
            sleeps=sleeps,
        )

    def test_budget_negative(self) -> None:
        sleeps: list[int] = []
        failures = run_required_aggregator(
            CHECK,
            "success",
            env=ENV,
            controls=SimulationControls(clock_step_ms=7200000),
            api_errors={"getWorkflowRun": [429]},
            api_error_detail={"getWorkflowRun": {"headers": {"retry-after": "301"}}},
            sleeps=sleeps,
        )
        self.assertEqual(len(failures), 1)
        self.assertTrue(failures[0].startswith("GitHub API rate limit:"))
        self.assertIn("No check result was judged", failures[0])
        self.assertNotIn(301000, sleeps)

    def test_budget_boundary(self) -> None:
        sleeps: list[int] = []
        failures = run_required_aggregator(
            CHECK,
            "success",
            env=ENV,
            controls=SimulationControls(clock_step_ms=7200000),
            api_errors={"getWorkflowRun": [429]},
            api_error_detail={"getWorkflowRun": {"headers": {"retry-after": "300"}}},
            sleeps=sleeps,
        )
        self.assertIn(300000, sleeps)
        self.assertEqual(len(failures), 1)
        self.assertIn("GitHub API time budget exhausted", failures[0])
        self.assertIn("No check result was judged", failures[0])

    def test_timeout_500_response_retries(self) -> None:
        sleeps: list[int] = []
        failures = run_required_aggregator(
            CHECK,
            "success",
            env=ENV,
            api_errors={"getWorkflowRun": [500]},
            api_error_detail={"getWorkflowRun": {"message": "TimeoutError: request timed out"}},
            sleeps=sleeps,
        )
        self.assertEqual(failures, [])
        self.assertEqual(sleeps, [30000])

    def test_truncated_api_bodies_retry_inside_each_api_closure(self) -> None:
        cases = (
            ("getWorkflowRun", "pull_request", None),
            ("listForRef", "pull_request", None),
            ("listWorkflowRunsForRepo", "push", None),
            ("listEventsForTimeline", "pull_request", "edited"),
        )
        for api_name, event, action in cases:
            with self.subTest(api=api_name):
                sleeps: list[int] = []
                failures = run_required_aggregator(
                    CHECK,
                    "success",
                    env=ENV,
                    event=event,
                    action=action,
                    pr_created_at=START if action == "edited" else None,
                    api_response_overrides={api_name: [{"data": ""}]},
                    sleeps=sleeps,
                )
                self.assertEqual(failures, [])
                self.assertIn(30000, sleeps)

    def test_final_sample_budget_failure_between_pages_is_not_judged(self) -> None:
        sleeps: list[int] = []
        failures = run_required_aggregator(
            CHECK,
            None,
            env=ENV,
            run_created_at=START,
            extra_checks=[check for check in pending("in_progress") if check["name"] == CHECK],
            controls=SimulationControls(
                advance_sleep=True,
                clock_step_ms=0,
                api_latency_after_ms=240 * 60 * 1000,
                api_latency_ms=5 * 60 * 1000,
            ),
            sleeps=sleeps,
        )
        self.assertEqual(len(failures), 1)
        self.assertIn("GitHub API time budget exhausted", failures[0])
        self.assertIn("No check result was judged", failures[0])
        self.assertNotIn(CHECK, failures[0])
        self.assertEqual(sum(sleeps), 240 * 60 * 1000)

    def test_mixed_retries_stop_at_wall_clock_budget(self) -> None:
        # The 62 successful samples through minute 237 include every API page.
        successful_pages = 62 * ((len(pending("in_progress")) + 99) // 100)
        for last_status, prefix in (
            (429, "GitHub API rate limit:"),
            (503, "Transient GitHub API failure:"),
        ):
            with self.subTest(last_status=last_status):
                sleeps: list[int] = []
                failures = run_required_aggregator(
                    CHECK,
                    None,
                    env=ENV,
                    run_created_at=START,
                    extra_checks=[
                        check for check in pending("in_progress") if check["name"] == CHECK
                    ],
                    controls=SimulationControls(advance_sleep=True, clock_step_ms=0),
                    api_errors={
                        "listForRef": [429]
                        + [None] * successful_pages
                        + [429, 503, 429, 502, last_status]
                    },
                    api_error_detail={"listForRef": {"headers": {"retry-after": "90"}}},
                    sleeps=sleeps,
                )
                self.assertEqual(len(failures), 1)
                self.assertTrue(failures[0].startswith(prefix), failures)
                self.assertEqual(sleeps[-4:], [90000, 30000, 90000, 60000])
                self.assertEqual(sum(sleeps), (244 * 60 + 30) * 1000)

    def test_mixed_retry_counts_are_bounded(self) -> None:
        for statuses, prefix in (
            ([429, 503] * 6 + [429], "GitHub API rate limit:"),
            ([503, 429] * 6 + [503], "Transient GitHub API failure:"),
        ):
            with self.subTest(statuses=statuses):
                sleeps: list[int] = []
                failures = run_required_aggregator(
                    CHECK,
                    "success",
                    env=ENV,
                    controls=SimulationControls(advance_sleep=True, clock_step_ms=0),
                    api_errors={"listForRef": statuses},
                    api_error_detail={"listForRef": {"headers": {"retry-after": "0"}}},
                    sleeps=sleeps,
                )
                self.assertEqual(len(failures), 1)
                self.assertTrue(failures[0].startswith(prefix), failures)
                self.assertEqual(len(sleeps), 12)
                self.assertEqual(sum(sleeps), 15 * 60 * 1000)

    def test_rate_limit_wait_budget_is_shared(self) -> None:
        sleeps: list[int] = []
        failures = run_required_aggregator(
            CHECK,
            "success",
            env=ENV,
            controls=SimulationControls(advance_sleep=True, clock_step_ms=0),
            api_errors={"getWorkflowRun": [429], "listForRef": [429]},
            api_error_detail={
                "getWorkflowRun": {"headers": {"retry-after": "5399"}},
                "listForRef": {"headers": {"retry-after": "2"}},
            },
            sleeps=sleeps,
        )
        self.assertTrue(failures[0].startswith("GitHub API rate limit:"))
        self.assertEqual(sleeps, [5399000])

    def test_startup_budget(self) -> None:
        sleeps: list[int] = []
        run_required_aggregator(
            CHECK,
            None,
            env=ENV,
            extra_checks=pending("queued"),
            controls=SimulationControls(advance_sleep=True),
            api_errors={"getWorkflowRun": [429]},
            api_error_detail={"getWorkflowRun": {"headers": {"retry-after": "5400"}}},
            sleeps=sleeps,
        )
        self.assertIn(5400000, sleeps)
        self.assertLess(len(sleeps), 20)
        self.assertLessEqual(sum(sleeps), 245 * 60 * 1000)

    def test_reread_after_later_wait(self) -> None:
        failures = run_required_aggregator(
            CHECK,
            None,
            env=ENV,
            event="push",
            later_checks=pending("completed"),
            api_errors={"listWorkflowRunsForRepo": [429]},
            api_error_detail={"listWorkflowRunsForRepo": {"headers": {"retry-after": "7"}}},
        )
        self.assertFalse(any(CHECK in f and "never" in f for f in failures))
        self.assertTrue(any(CHECK in f for f in failures))

    def test_fingerprint_ignores_unjudged_changes(self) -> None:
        sleeps: list[int] = []
        run_required_aggregator(
            CHECK,
            None,
            env=ENV,
            extra_checks=pending("queued"),
            controls=SimulationControls(changing_check="Unrelated job"),
            sleeps=sleeps,
        )
        self.assertEqual(sleeps[:3], [30000, 60000, 120000])

    def test_primary_limit(self) -> None:
        sleeps: list[int] = []
        self.assertEqual(self.primary(sleeps), [])
        self.assertEqual(len(sleeps), 1)
        self.assertTrue(MIN_WAIT_MS < sleeps[0] <= MAX_WAIT_MS)

    def test_retry_after(self) -> None:
        sleeps: list[int] = []
        failures = run_required_aggregator(
            CHECK,
            "success",
            env=ENV,
            event="push",
            api_errors={"listWorkflowRunsForRepo": [429]},
            api_error_detail={"listWorkflowRunsForRepo": {"headers": {"retry-after": "7"}}},
            sleeps=sleeps,
        )
        self.assertEqual(failures, [])
        self.assertIn(7000, sleeps)

    def test_secondary_limit(self) -> None:
        sleeps: list[int] = []
        failures = run_required_aggregator(
            CHECK,
            "success",
            env=ENV,
            api_errors={"getWorkflowRun": [403]},
            api_error_detail={
                "getWorkflowRun": {"message": "You have exceeded a secondary rate limit"}
            },
            sleeps=sleeps,
        )
        self.assertEqual(failures, [])
        self.assertIn(60000, sleeps)

    def test_permission_403(self) -> None:
        with self.assertRaises(CommandFailed):
            run_required_aggregator(
                CHECK,
                "success",
                env=ENV,
                api_errors={"listForRef": [403]},
                api_error_detail={
                    "listForRef": {"message": "Resource not accessible by integration"}
                },
            )

    def test_lasting_limit(self) -> None:
        failures = run_required_aggregator(
            CHECK,
            "success",
            env=ENV,
            run_created_at=START,
            api_always={"listForRef": 403},
            api_error_detail=detail(7200),
        )
        self.assertEqual(len(failures), 1)
        self.assertTrue(failures[0].startswith("GitHub API rate limit:"))
        self.assertIn("listAllCheckRuns", failures[0])

    def test_real_failure(self) -> None:
        failures = run_required_aggregator(
            CHECK,
            "failure",
            env=ENV,
            run_created_at=START,
            api_errors={"listForRef": [403]},
            api_error_detail=detail(3600),
        )
        self.assertEqual(len(failures), 1)
        self.assertIn(CHECK, failures[0])
        self.assertFalse(failures[0].startswith("GitHub API rate limit:"))

    def poll_sleeps(self, later: bool = False) -> list[int]:
        sleeps: list[int] = []
        run_required_aggregator(
            CHECK,
            None,
            env=ENV,
            extra_checks=pending("queued" if later else "in_progress"),
            later_checks=pending("in_progress") if later else (),
            sleeps=sleeps,
        )
        return sleeps

    def test_adaptive_polling(self) -> None:
        self.assertEqual(self.poll_sleeps()[:5], [30000, 60000, 120000, 240000, 240000])

    def test_changed_poll_resets(self) -> None:
        self.assertEqual(self.poll_sleeps(True)[:3], [30000, 30000, 60000])

    def final_sample(
        self,
        completed_after_s: int,
        *,
        retry_after: str = "90",
        sleep_overrun_after_ms: int | None = None,
    ) -> tuple[list[str], list[int]]:
        sleeps: list[int] = []
        failures = run_required_aggregator(
            CHECK,
            None,
            env=ENV,
            run_created_at=START,
            extra_checks=[check for check in pending("in_progress") if check["name"] == CHECK],
            later_checks=[{"name": CHECK, "conclusion": "success", "check_suite": {"id": 1}}]
            + [check for check in pending("completed") if check["name"] != CHECK],
            controls=SimulationControls(
                advance_sleep=True,
                clock_step_ms=0,
                later_checks_after_ms=completed_after_s * 1000,
                sleep_overrun_after_ms=sleep_overrun_after_ms,
            ),
            api_errors={"listForRef": [429]},
            api_error_detail={"listForRef": {"headers": {"retry-after": retry_after}}},
            sleeps=sleeps,
        )
        return failures, sleeps

    def test_final_sample_observes_completion_inside_deadline(self) -> None:
        failures, sleeps = self.final_sample(238 * 60)
        self.assertEqual(failures, [])
        self.assertEqual(sleeps[4:8], [240000] * 4)
        self.assertEqual(sum(sleeps[:-1]), 237 * 60 * 1000)
        self.assertEqual(sum(sleeps), 240 * 60 * 1000)
        self.assertGreaterEqual(sum(sleeps), 238 * 60 * 1000)

    def test_final_sample_observes_completion_in_last_second(self) -> None:
        failures, sleeps = self.final_sample(240 * 60 - 1)
        self.assertEqual(failures, [])
        self.assertEqual(sum(sleeps), 240 * 60 * 1000)

    def test_final_sample_survives_timer_overrun(self) -> None:
        failures, sleeps = self.final_sample(
            240 * 60 - 1,
            retry_after="29.999",
            sleep_overrun_after_ms=235 * 60 * 1000,
        )
        self.assertEqual(failures, [])
        self.assertEqual(sum(sleeps), 240 * 60 * 1000 - 1)

    def test_final_sample_does_not_wait_for_late_completion(self) -> None:
        failures, sleeps = self.final_sample(241 * 60)
        self.assertTrue(any(CHECK in failure and "in_progress" in failure for failure in failures))
        self.assertEqual(sum(sleeps), 240 * 60 * 1000)
        self.assertLess(len(sleeps), 70)

    def mutate(self, before: str, after: str) -> Path:
        text = harness.AGGREGATOR_PATH.read_text(encoding="utf-8")
        mutated = text.replace(before, after)
        self.assertNotEqual(text, mutated)
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, directory, True)
        copy = directory / "required-aggregator.yml"
        copy.write_text(mutated, encoding="utf-8")
        return copy

    def test_planted_no_rate_limit_retry(self) -> None:
        copy = self.mutate(
            "const rateLimitSeconds = rateLimitWaitSeconds(error, rateLimitWaits);",
            "const rateLimitSeconds = null;",
        )
        with mock.patch.object(harness, "AGGREGATOR_PATH", copy):
            with self.assertRaises(CommandFailed):
                self.primary([])

    def test_planted_no_final_sample(self) -> None:
        copy = self.mutate(
            "while (Date.now() < deadline || !finalSampleTaken)", "while (Date.now() < deadline)"
        )
        with mock.patch.object(harness, "AGGREGATOR_PATH", copy):
            failures, _ = self.final_sample(238 * 60)
        self.assertTrue(any(CHECK in failure and "in_progress" in failure for failure in failures))

    def test_planted_fixed_poll(self) -> None:
        copy = self.mutate("setTimeout(r, pollIntervalSeconds * 1000)", "setTimeout(r, 30000)")
        with mock.patch.object(harness, "AGGREGATOR_PATH", copy):
            self.assertNotEqual(self.poll_sleeps()[:5], [30000, 60000, 120000, 240000, 240000])


if __name__ == "__main__":
    unittest.main()
