#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""An owed required context passes the aggregator only by succeeding (ADR-3293).

These cases run the real embedded script of
.github/workflows/required-aggregator.yml. Three exemptions remain, each listed
by name in the script: contexts the tier does not owe (ADR-2169), the
pull-request-only contexts on a push event, and disabled hardware lanes. Every
other required context that is absent, skipped or neutral fails.

AGGREGATOR_UNDER_TEST points the cases at another copy of the workflow, which
is how the planted proof runs them against the aggregator before ADR-3293.
"""

from __future__ import annotations

import contextlib
import json
import os
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import required_aggregator_harness as harness
from scripts.ci.required_aggregator_harness import run_required_aggregator

REPO_ROOT = Path(__file__).resolve().parents[3]
CONFIG = json.loads((REPO_ROOT / ".github" / "ci-tier.json").read_text(encoding="utf-8"))
PLAIN_OWED = ("Ubuntu gcc", "go vet + go test")  # owed in the full tier, not strict
PUSH_SUITE = 1
PR_SUITE = 7
MASTER_PUSH_RUN = {"event": "push", "head_branch": "master", "check_suite_id": PUSH_SUITE}

_OVERRIDE = contextlib.ExitStack()


def setUpModule() -> None:
    override = os.environ.get("AGGREGATOR_UNDER_TEST")
    if override:
        _OVERRIDE.enter_context(mock.patch.object(harness, "AGGREGATOR_PATH", Path(override)))


def tearDownModule() -> None:
    _OVERRIDE.close()


def pull_request_only() -> list[str]:
    text = harness.AGGREGATOR_PATH.read_text(encoding="utf-8")
    block = re.search(r"const pullRequestOnly = \[(.*?)\];", text, re.S)
    if block is None:
        raise AssertionError("the aggregator must declare pullRequestOnly")
    body = re.sub(r"(?m)^\s*//.*$", "", block.group(1))
    return re.findall(r"'([^']+)'", body)


def light_env() -> dict[str, str]:
    return {
        "CI_TIER": "light",
        "CI_TIER_ALWAYS": json.dumps(CONFIG["always"]),
        "CI_TIER_FULL_ONLY": json.dumps(CONFIG["full_only"]),
    }


class OwedContextMustSucceed(unittest.TestCase):
    def test_absent_owed_context_fails_on_a_pull_request(self) -> None:
        for name in PLAIN_OWED:
            with self.subTest(name=name):
                failures = run_required_aggregator(name, None)
                self.assertEqual(len(failures), 1, failures)
                self.assertIn("never reported", failures[0])

    def test_skipped_or_neutral_owed_context_fails(self) -> None:
        for name in PLAIN_OWED:
            for conclusion in ("skipped", "neutral", "cancelled"):
                with self.subTest(name=name, conclusion=conclusion):
                    failures = run_required_aggregator(name, conclusion)
                    self.assertEqual(len(failures), 1, failures)
                    self.assertIn(conclusion, failures[0])

    def test_absent_owed_context_fails_on_a_master_push(self) -> None:
        for name in PLAIN_OWED:
            with self.subTest(name=name):
                failures = run_required_aggregator(
                    name, None, event="push", workflow_runs=[MASTER_PUSH_RUN]
                )
                self.assertEqual(len(failures), 1, failures)
                self.assertIn("never reported", failures[0])

    def test_successful_owed_context_passes(self) -> None:
        for name in PLAIN_OWED:
            with self.subTest(name=name):
                self.assertEqual(run_required_aggregator(name, "success"), [])


class NamedExemptionsStillPass(unittest.TestCase):
    def test_pull_request_only_contexts_may_be_absent_or_skipped_on_a_push(self) -> None:
        for name in pull_request_only():
            for conclusion in (None, "skipped"):
                with self.subTest(name=name, conclusion=conclusion):
                    failures = run_required_aggregator(
                        name, conclusion, event="push", workflow_runs=[MASTER_PUSH_RUN]
                    )
                    self.assertEqual(failures, [])

    def test_pull_request_only_contexts_are_owed_on_a_pull_request(self) -> None:
        for name in pull_request_only():
            with self.subTest(name=name):
                failures = run_required_aggregator(name, None)
                self.assertEqual(len(failures), 1, failures)

    def test_a_failed_pull_request_only_context_still_blocks_a_push(self) -> None:
        for name in pull_request_only():
            with self.subTest(name=name):
                failures = run_required_aggregator(
                    name,
                    "failure",
                    event="push",
                    workflow_runs=[MASTER_PUSH_RUN],
                    selected_suite=PUSH_SUITE,
                )
                self.assertEqual(len(failures), 1, failures)

    def test_contexts_the_light_tier_does_not_owe_may_be_absent(self) -> None:
        name = "Ubuntu gcc"
        self.assertIn(name, CONFIG["full_only"])
        self.assertEqual(run_required_aggregator(name, None, env=light_env()), [])


if __name__ == "__main__":
    unittest.main()
