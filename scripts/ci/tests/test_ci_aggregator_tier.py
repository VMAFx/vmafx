#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Required Checks Aggregator owes each tier only its own contexts (ADR-2169).

These cases run the real embedded script of
.github/workflows/required-aggregator.yml with the tier environment the
workflow's "Decide the tier" step would produce from .github/ci-tier.json.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import required_aggregator_harness as harness
from scripts.ci.required_aggregator_harness import run_required_aggregator

CONFIG = json.loads(
    (Path(__file__).resolve().parents[3] / ".github" / "ci-tier.json").read_text(encoding="utf-8")
)
RELEASE_REF = "release-please--branches--master--components--vmafx"
STRICT_FULL_ONLY = "Windows ARM64 MSVC"  # strictMustReport, and a full-tier context
PLAIN_FULL_ONLY = "Ubuntu gcc"  # not strict, a full-tier context
LIGHT_STRICT = "Standards & Invariant Verification Gate"  # strictMustReport, a light context
LIGHT_PLAIN = "Pre-Commit"
RELEASE_CONTRACT = "Release Script Contract"


def tier_env(tier: str) -> dict[str, str]:
    return {
        "CI_TIER": tier,
        "CI_TIER_ALWAYS": json.dumps(CONFIG["always"]),
        "CI_TIER_FULL_ONLY": json.dumps(CONFIG["full_only"]),
    }


def run(name: str, conclusion: str | None, tier: str | None, **kwargs: object) -> list[str]:
    env = {} if tier is None else tier_env(tier)
    return run_required_aggregator(name, conclusion, env=env, **kwargs)  # type: ignore[arg-type]


class LightTier(unittest.TestCase):
    def test_full_only_contexts_may_be_absent_or_skipped(self) -> None:
        for name in (STRICT_FULL_ONLY, PLAIN_FULL_ONLY, "Coverage Gate", "Docker Image Build"):
            for conclusion in (None, "skipped"):
                with self.subTest(name=name, conclusion=conclusion):
                    self.assertEqual(run(name, conclusion, "light"), [])

    def test_the_same_absence_fails_in_the_full_tier(self) -> None:
        """The relaxation is the tier's, not a general loosening."""
        self.assertEqual(len(run(STRICT_FULL_ONLY, None, "full")), 1)
        self.assertEqual(len(run(STRICT_FULL_ONLY, None, None)), 1)

    def test_a_full_only_context_that_ran_must_still_pass(self) -> None:
        failures = run(PLAIN_FULL_ONLY, "failure", "light")
        self.assertEqual(len(failures), 1)
        self.assertIn("does not owe it", failures[0])

    def test_owed_contexts_are_still_required(self) -> None:
        self.assertEqual(len(run(LIGHT_STRICT, None, "light")), 1)
        self.assertEqual(len(run(LIGHT_PLAIN, "failure", "light")), 1)
        self.assertEqual(run(LIGHT_PLAIN, "success", "light"), [])

    def test_the_release_contract_is_owed(self) -> None:
        self.assertEqual(len(run(RELEASE_CONTRACT, "failure", "light")), 1)


class ReleaseLightTier(unittest.TestCase):
    def test_only_the_release_contract_is_owed(self) -> None:
        for name in ("Netflix CPU Golden", "Ubuntu gcc+DNN", LIGHT_PLAIN, LIGHT_STRICT):
            with self.subTest(name=name):
                self.assertEqual(run(name, None, "release-light", head_ref=RELEASE_REF), [])
        failures = run(RELEASE_CONTRACT, None, "release-light", head_ref=RELEASE_REF)
        self.assertEqual(len(failures), 1)
        self.assertIn("never reported on SHA", failures[0])

    def test_a_context_that_ran_and_failed_still_blocks(self) -> None:
        failures = run(LIGHT_PLAIN, "failure", "release-light", head_ref=RELEASE_REF)
        self.assertEqual(len(failures), 1)

    def test_the_cut_label_restores_the_release_must_report_list(self) -> None:
        """With the cut label the tier is full and the three release contexts must report."""
        for name in ("Netflix CPU Golden", "Ubuntu gcc+DNN", RELEASE_CONTRACT):
            with self.subTest(name=name):
                failures = run(name, None, "full", head_ref=RELEASE_REF)
                self.assertEqual(len(failures), 1)
                self.assertIn("never reported", failures[0])


class TierDecisionIsChecked(unittest.TestCase):
    def test_a_failed_tier_job_blocks_the_merge(self) -> None:
        tier_job = {
            "name": "CI tier / Decide the CI tier",
            "conclusion": "failure",
            "check_suite": {"id": 1},
        }
        failures = run(LIGHT_PLAIN, "success", "light", extra_checks=[tier_job])
        self.assertEqual(len(failures), 1)
        self.assertIn("tier decision", failures[0])

    def test_a_skipped_or_successful_tier_job_does_not(self) -> None:
        for conclusion in ("success", "skipped"):
            tier_job = {
                "name": "CI tier / Decide the CI tier",
                "conclusion": conclusion,
                "check_suite": {"id": 1},
            }
            with self.subTest(conclusion=conclusion):
                self.assertEqual(run(LIGHT_PLAIN, "success", "light", extra_checks=[tier_job]), [])


TIER_NAME = "CI tier (Tests) / Decide the CI tier"


def every_required_check_passes() -> list[dict[str, object]]:
    script = harness._embedded_script(harness.AGGREGATOR_PATH.read_text(encoding="utf-8"))
    return [
        {"name": name, "conclusion": "success", "check_suite": {"id": 1}}
        for name in harness._required_names(script)
    ]


class TierDecisionPending(unittest.TestCase):
    """GitHub creates a job's check run only after the jobs it needs have completed."""

    def pending_then_complete(self) -> list[str]:
        queued_tier = {"name": TIER_NAME, "status": "queued", "check_suite": {"id": 1}}
        done_tier = {"name": TIER_NAME, "conclusion": "success", "check_suite": {"id": 1}}
        return run(
            LIGHT_STRICT,
            None,  # not reported in the first listing
            "light",
            extra_checks=[queued_tier],
            later_checks=[*every_required_check_passes(), done_tier],
        )

    def test_a_queued_tier_decision_keeps_the_aggregator_waiting(self) -> None:
        self.assertEqual(self.pending_then_complete(), [])

    def test_without_a_tier_decision_the_same_absence_fails(self) -> None:
        failures = run(LIGHT_STRICT, None, "light", later_checks=every_required_check_passes())
        self.assertEqual(len(failures), 1)
        self.assertIn("never reported", failures[0])

    def test_the_wait_is_what_makes_it_pass(self) -> None:
        """Planted defect: an aggregator that ignores a pending tier decision fails the case."""
        text = harness.AGGREGATOR_PATH.read_text(encoding="utf-8")
        mutated = text.replace(
            "delayedMissing.length === 0 && !tierPending &&", "delayedMissing.length === 0 &&"
        )
        self.assertNotEqual(text, mutated)
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, directory, True)
        copy = directory / "required-aggregator.yml"
        copy.write_text(mutated, encoding="utf-8")
        with mock.patch.object(harness, "AGGREGATOR_PATH", copy):
            self.assertEqual(len(self.pending_then_complete()), 1)


if __name__ == "__main__":
    unittest.main()
