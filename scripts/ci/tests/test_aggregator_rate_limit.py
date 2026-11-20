#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise rate-limit recovery and adaptive polling against the real script."""

from __future__ import annotations

import json
import shutil
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from unittest import mock

from scripts.ci import required_aggregator_harness as harness
from scripts.ci.required_aggregator_harness import run_required_aggregator
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
            "headers": {"x-ratelimit-remaining": "0", "x-ratelimit-reset": str(EPOCH + seconds)}
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
            harness._decode_driver_output('{"failures": [], "sleeps": [1, 2]}', None, sleeps), []
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
            harness._decode_driver_output('{"failures": [], "sleeps": []}', None, sleeps), []
        )
        self.assertEqual(sleeps, [3])

    def test_none_sleeps_ignores_payload(self) -> None:
        self.assertEqual(
            harness._decode_driver_output('{"failures": [], "sleeps": [true]}', None, None), []
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

    def test_planted_fixed_poll(self) -> None:
        copy = self.mutate("setTimeout(r, pollIntervalSeconds * 1000)", "setTimeout(r, 30000)")
        with mock.patch.object(harness, "AGGREGATOR_PATH", copy):
            self.assertNotEqual(self.poll_sleeps()[:5], [30000, 60000, 120000, 240000, 240000])


if __name__ == "__main__":
    unittest.main()
