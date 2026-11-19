#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The recorded upstream head and the CI job that reads it (ADR-1474)."""

from __future__ import annotations

import contextlib
import io
import re
import sys
import unittest
from collections.abc import Sequence
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import upstream_parity_pin as pin  # noqa: E402

HEADING = "## Upstream head the fork is at parity with"
FULL = "cea2b4d832a105116a3f16f56d6f5d953421952c"
JOB_NAME = "Licence Provenance"


def document(*headings: str) -> str:
    body = "# Known upstream bugs\n\nIntro.\n\n## Something else: `abcdef1`\n\nText.\n"
    return body + "".join(f"\n{heading}\n\nText.\n" for heading in headings)


class RecordedPin(unittest.TestCase):
    def test_the_heading_gives_the_id(self) -> None:
        text = document(f"{HEADING}: `cea2b4d83` (2026-10-02)")
        self.assertEqual(pin.recorded_pin(text), "cea2b4d83")

    def test_the_date_is_optional_and_a_full_id_is_accepted(self) -> None:
        self.assertEqual(pin.recorded_pin(document(f"{HEADING}: `{FULL}`")), FULL)

    def test_id_length_bounds(self) -> None:
        self.assertEqual(pin.recorded_pin(document(f"{HEADING}: `{FULL[:7]}`")), FULL[:7])
        for bad in (FULL[:6], FULL + "0"):
            with self.subTest(length=len(bad)), self.assertRaises(pin.PinError):
                pin.recorded_pin(document(f"{HEADING}: `{bad}`"))

    def test_no_heading_is_an_error(self) -> None:
        with self.assertRaisesRegex(pin.PinError, "no heading"):
            pin.recorded_pin(document())

    def test_two_headings_are_an_error(self) -> None:
        text = document(f"{HEADING}: `cea2b4d83`", f"{HEADING}: `9e48141bd`")
        with self.assertRaisesRegex(pin.PinError, "2 headings"):
            pin.recorded_pin(text)

    def test_a_reworded_heading_is_an_error_not_a_miss(self) -> None:
        for heading in (
            f"{HEADING}: cea2b4d83 (2026-10-02)",  # no code span
            f"{HEADING}: `CEA2B4D83`",  # git prints lowercase
            f"{HEADING}: `cea2b4d83`, checked 2026-10-02",
            f"{HEADING} (`cea2b4d83`)",
        ):
            with self.subTest(heading=heading), self.assertRaisesRegex(pin.PinError, "malformed"):
                pin.recorded_pin(document(heading))

    def test_a_deeper_or_quoted_heading_does_not_count(self) -> None:
        text = document(f"{HEADING}: `cea2b4d83`") + f"\n#{HEADING}: `9e48141bd`\n> {HEADING}\n"
        self.assertEqual(pin.recorded_pin(text), "cea2b4d83")

    def test_the_repository_records_exactly_one_head(self) -> None:
        recorded = pin.recorded_pin((ROOT / pin.RECORD).read_text(encoding="utf-8"))
        self.assertRegex(recorded, r"^[0-9a-f]{7,40}$")


class Resolve(unittest.TestCase):
    def runner(self, answers: dict[str, tuple[int, str]]) -> pin.GitRunner:
        self.calls: list[list[str]] = []

        def call(args: Sequence[str]) -> tuple[int, str]:
            self.calls.append(list(args))
            return answers[args[0]]

        return call

    def test_an_upstream_commit_resolves_to_its_full_id(self) -> None:
        call = self.runner({"rev-parse": (0, FULL), "merge-base": (0, "")})
        self.assertEqual(pin.resolve("cea2b4d83", "FETCH_HEAD", call), FULL)
        self.assertEqual(
            self.calls,
            [
                ["rev-parse", "--verify", "--quiet", "cea2b4d83^{commit}"],
                ["merge-base", "--is-ancestor", FULL, "FETCH_HEAD"],
            ],
        )

    def test_an_unknown_or_ambiguous_id_is_refused(self) -> None:
        for answer in ((1, ""), (0, ""), (0, "not a commit id")):
            call = self.runner({"rev-parse": answer})
            with self.subTest(answer=answer), self.assertRaisesRegex(pin.PinError, "exactly one"):
                pin.resolve("cea2b4d83", "FETCH_HEAD", call)

    def test_a_commit_outside_the_upstream_branch_is_refused(self) -> None:
        call = self.runner({"rev-parse": (0, FULL), "merge-base": (1, "")})
        with self.assertRaisesRegex(pin.PinError, "not an upstream commit"):
            pin.resolve("cea2b4d83", "FETCH_HEAD", call)


class CommandLine(unittest.TestCase):
    def test_a_missing_record_exits_2(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            status = pin.main(["--repo", str(ROOT / "scripts/ci/tests/no-such-checkout")])
        self.assertEqual(status, 2)
        self.assertIn("upstream_parity_pin:", stderr.getvalue())

    def test_the_recorded_id_is_printed(self) -> None:
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = pin.main([])
        self.assertEqual(status, 0)
        self.assertRegex(stdout.getvalue(), r"^[0-9a-f]{7,40}\n$")


class SharedUpstreamUrl(unittest.TestCase):
    """The default is declared once; workflows and the parity script consume it."""

    def test_the_default_is_declared_in_build_config(self) -> None:
        config = (ROOT / "build-config.env").read_text(encoding="utf-8")
        self.assertIn('NETFLIX_VMAF_URL="https://github.com/Netflix/vmaf.git"', config)

    def test_the_upstream_consumers_do_not_repeat_the_literal(self) -> None:
        for rel in (
            ".github/workflows/lint-and-format.yml",
            ".github/workflows/upstream-watcher.yml",
            ".github/workflows/ffmpeg-integration.yml",
        ):
            with self.subTest(workflow=rel):
                text = (ROOT / rel).read_text(encoding="utf-8")
                self.assertNotIn("https://github.com/Netflix/vmaf.git", text)
                self.assertIn("${NETFLIX_VMAF_URL}", text)
                self.assertIn('[ -z "${NETFLIX_VMAF_URL:-}" ]', text)


class Workflow(unittest.TestCase):
    """The job is only a gate if it has history, the pin, and a required name."""

    def job(self) -> str:
        text = (ROOT / ".github/workflows/lint-and-format.yml").read_text(encoding="utf-8")
        match = re.search(r"(?ms)^  licence-provenance:\n(.*?)(?=^  [a-z0-9-]+:\n|\Z)", text)
        self.assertIsNotNone(match, "lint-and-format.yml has no licence-provenance job")
        return match.group(1) if match else ""

    def test_the_job_has_full_history_on_both_sides(self) -> None:
        job = self.job()
        self.assertIn("fetch-depth: 0", job)
        self.assertIn(". ./build-config.env", job)
        self.assertIn('git fetch --no-tags --quiet "${NETFLIX_VMAF_URL}" master', job)
        self.assertNotIn("--depth", job)

    def test_the_job_checks_against_the_recorded_head(self) -> None:
        job = self.job()
        self.assertIn("scripts/ci/upstream_parity_pin.py --within FETCH_HEAD", job)
        self.assertIn(
            'scripts/dev/relicense_fork_files.py --check --upstream-ref "${UPSTREAM_SHA}"', job
        )
        self.assertIn("UPSTREAM_SHA: ${{ steps.upstream.outputs.sha }}", job)
        self.assertNotIn("upstream/master", job)

    def test_the_job_always_reports_and_is_required(self) -> None:
        job = self.job()
        self.assertRegex(job, rf"(?m)^    # required-aggregator\n    name: {JOB_NAME}$")
        self.assertNotIn("continue-on-error", job)
        self.assertNotIn("paths:", job)
        aggregator = (ROOT / ".github/workflows/required-aggregator.yml").read_text(
            encoding="utf-8"
        )
        for array in ("required", "strictMustReport"):
            match = re.search(rf"const {array} = \[(.*?)\];", aggregator, re.DOTALL)
            self.assertIsNotNone(match, array)
            with self.subTest(array=array):
                self.assertIn(f"'{JOB_NAME}'", match.group(1) if match else "")

    def test_the_tests_of_what_the_job_relies_on_run_in_the_tooling_suite(self) -> None:
        # A test runs once in CI (ADR-1568): Tooling Tests runs both files on
        # every pull request and push; the job runs the check itself.
        from scripts.ci.suite_registry import suite_members  # noqa: PLC0415

        tooling = suite_members(ROOT, "tooling")
        self.assertIn("scripts/dev/tests/test_relicense_fork_files.py", tooling)
        self.assertIn("scripts/ci/tests/test_upstream_parity_pin.py", tooling)
        job = self.job()
        self.assertNotIn("scripts/dev/tests/test_relicense_fork_files.py", job)


if __name__ == "__main__":
    unittest.main()
