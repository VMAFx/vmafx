#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for scripts/ci/ci_tier.py (ADR-2169)."""

from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import ci_tier

CONFIG = ci_tier.load_config()
REPO = "VMAFx/vmafx"
RELEASE_REF = "release-please--branches--master--components--vmafx"


def event(**changes: object) -> ci_tier.Event:
    base = {
        "name": "pull_request",
        "repository": REPO,
        "head_repository": REPO,
        "head_ref": "fix/example",
        "author": "someone",
        "author_type": "User",
        "labels": (),
    }
    return ci_tier.Event(**{**base, **changes})  # type: ignore[arg-type]


def decide(facts: ci_tier.Event, *, exempt: bool = False) -> ci_tier.Decision:
    return ci_tier.decide(facts, CONFIG, release_exempt=lambda: exempt)


class Decide(unittest.TestCase):
    def test_not_a_pull_request_is_full(self) -> None:
        for name in ("push", "schedule", "workflow_dispatch", "release"):
            self.assertEqual(decide(event(name=name)).tier, ci_tier.FULL, name)

    def test_own_pull_request_is_light(self) -> None:
        self.assertEqual(decide(event()).tier, ci_tier.LIGHT)

    def test_fork_pull_request_is_full(self) -> None:
        self.assertEqual(decide(event(head_repository="x/vmafx")).tier, ci_tier.FULL)

    def test_missing_head_repository_is_full(self) -> None:
        """A payload without a head repository (a deleted fork) is not trusted as own."""
        self.assertEqual(decide(event(head_repository="")).tier, ci_tier.FULL)

    def test_full_label_is_full(self) -> None:
        self.assertEqual(decide(event(labels=("ci: full",))).tier, ci_tier.FULL)

    def test_release_branch_needs_the_exemption_to_be_light(self) -> None:
        facts = event(head_ref=RELEASE_REF)
        self.assertEqual(decide(facts, exempt=True).tier, ci_tier.RELEASE_LIGHT)
        self.assertEqual(decide(facts, exempt=False).tier, ci_tier.LIGHT)

    def test_cut_label_makes_the_release_pull_request_full(self) -> None:
        facts = event(head_ref=RELEASE_REF, labels=("autorelease: cut",))
        self.assertEqual(decide(facts, exempt=True).tier, ci_tier.FULL)

    def test_cut_label_elsewhere_changes_nothing(self) -> None:
        self.assertEqual(decide(event(labels=("autorelease: cut",))).tier, ci_tier.LIGHT)

    def test_exemption_is_asked_only_for_a_release_branch(self) -> None:
        def refuse() -> bool:
            raise AssertionError("asked for an ordinary pull request")

        self.assertEqual(ci_tier.decide(event(), CONFIG, release_exempt=refuse).tier, "light")

    def test_outputs_are_derived_from_the_tier(self) -> None:
        light = decide(event()).outputs()
        self.assertEqual((light["light"], light["full"]), ("true", "false"))
        full = decide(event(name="push")).outputs()
        self.assertEqual((full["light"], full["full"]), ("true", "true"))
        release = decide(event(head_ref=RELEASE_REF), exempt=True).outputs()
        self.assertEqual((release["light"], release["full"]), ("false", "false"))


class ReleaseExemption(unittest.TestCase):
    """The real release-pr-exempt.sh decides who the release pull request is."""

    def test_bot_author_on_a_release_branch_is_exempt(self) -> None:
        facts = event(head_ref=RELEASE_REF, author="github-actions[bot]", author_type="Bot")
        self.assertTrue(ci_tier.release_exempt_from_script(facts, ["x"]))

    def test_a_person_on_a_release_branch_is_not(self) -> None:
        facts = event(head_ref=RELEASE_REF, author="someone")
        self.assertFalse(
            ci_tier.release_exempt_from_script(facts, [".release-please-manifest.json"])
        )

    def test_the_maintainer_account_is_exempt_only_for_a_release_diff(self) -> None:
        facts = event(head_ref=RELEASE_REF, author="lusoris")
        self.assertTrue(
            ci_tier.release_exempt_from_script(facts, [".release-please-manifest.json"])
        )
        self.assertFalse(ci_tier.release_exempt_from_script(facts, ["core/src/feature/ssim.c"]))


class LiveLabels(unittest.TestCase):
    def test_labels_are_read_from_the_api_with_paging(self) -> None:
        pages = {
            1: [{"name": f"l{i}"} for i in range(ci_tier.PAGE_SIZE)],
            2: [{"name": "ci: full"}],
        }

        def fetch(path: str) -> object:
            return pages[int(path.rsplit("page=", 1)[1])]

        labels = ci_tier.live_labels(fetch, REPO, "7")
        self.assertEqual(len(labels), ci_tier.PAGE_SIZE + 1)
        self.assertIn("ci: full", labels)

    def test_a_non_list_answer_is_refused(self) -> None:
        with self.assertRaises(ci_tier.TierError):
            ci_tier.live_labels(lambda path: {"message": "Not Found"}, REPO, "7")

    def test_live_labels_beat_the_payload(self) -> None:
        env = {
            "EVENT_NAME": "pull_request",
            "GITHUB_REPOSITORY": REPO,
            "HEAD_REPOSITORY": REPO,
            "HEAD_REF": "fix/example",
            "PR_NUMBER": "7",
            "PR_LABELS": "[]",
        }
        facts = ci_tier.event_from_environment(env, lambda path: [{"name": "ci: full"}])
        self.assertEqual(facts.labels, ("ci: full",))

    def test_payload_labels_are_used_without_a_token_and_say_so(self) -> None:
        env = {"EVENT_NAME": "pull_request", "PR_NUMBER": "7", "PR_LABELS": '["a"]'}
        err = io.StringIO()
        with redirect_stderr(err):
            facts = ci_tier.event_from_environment(env, None)
        self.assertEqual(facts.labels, ("a",))
        self.assertIn("event payload", err.getvalue())

    def test_a_null_payload_means_no_labels(self) -> None:
        facts = ci_tier.event_from_environment({"EVENT_NAME": "push", "PR_LABELS": "null"}, None)
        self.assertEqual(facts.labels, ())


class Api(unittest.TestCase):
    def test_a_non_https_url_is_refused(self) -> None:
        for url in ("http://api.github.com/x", "file:///etc/passwd", "ftp://h/x"):
            with self.subTest(url=url), self.assertRaises(ci_tier.TierError):
                ci_tier.api_request("GET", url, "t")


class Config(unittest.TestCase):
    def write(self, text: str) -> Path:
        directory = tempfile.mkdtemp()
        path = Path(directory) / "ci-tier.json"
        path.write_text(text, encoding="utf-8")
        self.addCleanup(path.unlink)
        return path

    def test_a_name_in_both_lists_is_refused(self) -> None:
        config = {**CONFIG, "always": ["A"], "full_only": ["A"]}
        with self.assertRaises(ci_tier.TierError):
            ci_tier.load_config(self.write(json.dumps(config)))

    def test_a_missing_key_is_refused(self) -> None:
        with self.assertRaises(ci_tier.TierError):
            ci_tier.load_config(self.write('{"labels": {}}'))

    def test_an_own_input_lane_must_name_a_full_only_context(self) -> None:
        lane = {
            "workflow": "w.yml",
            "context": "Not Full Only",
            "selectors": ["s"],
            "reason": "r",
        }
        config = {**CONFIG, "own_input_lanes": [lane]}
        with self.assertRaises(ci_tier.TierError):
            ci_tier.load_config(self.write(json.dumps(config)))

    def test_an_own_input_lane_needs_selectors_and_a_reason(self) -> None:
        context = CONFIG["full_only"][0]
        for broken in (
            {"workflow": "w.yml", "context": context, "selectors": [], "reason": "r"},
            {"workflow": "w.yml", "context": context, "selectors": ["s"], "reason": " "},
            {"context": context, "selectors": ["s"], "reason": "r"},
        ):
            with self.subTest(lane=broken):
                config = {**CONFIG, "own_input_lanes": [broken]}
                with self.assertRaises(ci_tier.TierError):
                    ci_tier.load_config(self.write(json.dumps(config)))

    def test_the_declared_own_input_lanes_load(self) -> None:
        loaded = ci_tier.load_config(ci_tier.DEFAULT_CONFIG)
        self.assertTrue(loaded["own_input_lanes"])


class Main(unittest.TestCase):
    def test_a_broken_definition_fails_the_job_loudly(self) -> None:
        directory = tempfile.mkdtemp()
        output = Path(directory) / "out"
        broken = Path(directory) / "broken.json"
        broken.write_text("{}", encoding="utf-8")
        env = {"EVENT_NAME": "pull_request"}
        with mock.patch.dict(os.environ, env, clear=True), redirect_stdout(io.StringIO()):
            with redirect_stderr(io.StringIO()), self.assertRaises(ci_tier.TierError):
                ci_tier.main(["--config", str(broken), "--github-output", str(output)])

    def test_main_writes_the_outputs_and_the_lists_the_aggregator_reads(self) -> None:
        directory = tempfile.mkdtemp()
        output = Path(directory) / "out"
        env = {
            "EVENT_NAME": "pull_request",
            "GITHUB_REPOSITORY": REPO,
            "HEAD_REPOSITORY": REPO,
            "HEAD_REF": "fix/example",
            "PR_LABELS": "[]",
        }
        with mock.patch.dict(os.environ, env, clear=True), redirect_stdout(io.StringIO()):
            self.assertEqual(ci_tier.main(["--github-output", str(output)]), 0)
        written = dict(line.split("=", 1) for line in output.read_text().splitlines())
        self.assertEqual(written["tier"], "light")
        self.assertEqual(json.loads(written["always_json"]), CONFIG["always"])
        self.assertEqual(json.loads(written["full_only_json"]), CONFIG["full_only"])

    def test_a_failing_decision_falls_back_to_the_full_tier(self) -> None:
        directory = tempfile.mkdtemp()
        output = Path(directory) / "out"
        env = {
            "EVENT_NAME": "pull_request",
            "GH_TOKEN": "t",
            "PR_NUMBER": "7",
            "GITHUB_REPOSITORY": REPO,
        }
        boom = ci_tier.TierError("api down")
        with (
            mock.patch.dict(os.environ, env, clear=True),
            mock.patch.object(ci_tier, "api_request", side_effect=boom),
            redirect_stdout(io.StringIO()),
            redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(ci_tier.main(["--github-output", str(output)]), 0)
        self.assertIn("tier=full", output.read_text())


if __name__ == "__main__":
    unittest.main()
