#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cases for scripts/ci/check_fixture_bundle_jobs.py."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "check_fixture_bundle_jobs.py"
SPEC = importlib.util.spec_from_file_location("check_fixture_bundle_jobs", SCRIPT)
assert SPEC and SPEC.loader
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)

TEST_FILE = 'BUNDLE = ("yuv", "pq-sollevante-v1")\nOTHER = ("yuv", "hdr-ictcp-pixels-v1")\n'
FETCH = "      - uses: ./.github/actions/pq-fixture\n        with:\n          phase: get\n"
FETCH_ICTCP = FETCH + "          bundle: hdr-ictcp-pixels-v1\n"


def workflow(steps: str) -> str:
    """A one-job workflow with the given step lines."""
    return "on: push\njobs:\n  t:\n    runs-on: ubuntu-latest\n    steps:\n" + steps


class FixtureBundleJobs(unittest.TestCase):
    def _check(self, steps: str, scripts: dict[str, str] | None = None) -> list[str]:
        """Run the check on a temporary tree with one test file and one workflow."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "python" / "test").mkdir(parents=True)
            (root / "python" / "test" / "hdr_pin_test.py").write_text(TEST_FILE)
            (root / "python" / "test" / "plain_test.py").write_text("X = ('yuv', 'a.yuv')\n")
            (root / ".github" / "workflows").mkdir(parents=True)
            (root / ".github" / "workflows" / "w.yml").write_text(workflow(steps))
            for rel, text in (scripts or {}).items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(text)
            problems: list[str] = MOD.check(root)
            return problems

    def test_job_fetching_every_bundle_passes(self) -> None:
        """Positive: a whole-suite job that fetches both bundles has no gap."""
        run = "      - run: python3 -m pytest python/test/ -v\n"
        self.assertEqual(self._check(FETCH + FETCH_ICTCP + run), [])

    def test_whole_suite_without_fetch_fails(self) -> None:
        """Negative: pytest over python/test without the fetch reports both bundles."""
        problems = self._check("      - run: python3 -m pytest python/test/ -v\n")
        self.assertEqual(len(problems), 2)
        self.assertIn("hdr-ictcp-pixels-v1 (hdr_pin_test)", problems[0])
        self.assertIn("pq-sollevante-v1 (hdr_pin_test)", problems[1])

    def test_tox_without_one_bundle_fails(self) -> None:
        """Negative: tox runs the whole suite; fetching only the default bundle misses one."""
        problems = self._check(FETCH + "      - run: tox -c python\n")
        self.assertEqual(problems, ["w.yml:t: does not fetch hdr-ictcp-pixels-v1 (hdr_pin_test)"])

    def test_called_script_running_the_file_counts(self) -> None:
        """Negative: a run step that calls a scripts/ci script naming the file is a test run."""
        script = {"scripts/ci/run-pins.sh": "python3 -m pytest python/test/hdr_pin_test.py\n"}
        problems = self._check("      - run: bash scripts/ci/run-pins.sh out.xml\n", script)
        self.assertEqual(len(problems), 2)

    def test_job_running_another_file_needs_no_fetch(self) -> None:
        """Boundary: a job naming only a file that reads no bundle is not a gap."""
        run = "      - run: python3 -m pytest python/test/plain_test.py\n"
        self.assertEqual(self._check(run), [])

    def test_resource_path_mention_is_not_a_suite_run(self) -> None:
        """Boundary: python/test/resource in a cache path is not a run of the suite."""
        run = "      - run: bash scripts/ci/prune.sh python/test/resource\n"
        self.assertEqual(self._check(run), [])

    def test_finish_phase_alone_does_not_fetch(self) -> None:
        """Boundary: a finish step is no fetch."""
        finish = (
            "      - uses: ./.github/actions/pq-fixture\n        with:\n          phase: finish\n"
        )
        problems = self._check(finish + "      - run: tox -c python\n")
        self.assertEqual(len(problems), 2)

    def test_repository_workflows_fetch_their_bundles(self) -> None:
        """The repository's own workflows have no gap."""
        self.assertEqual(MOD.check(SCRIPT.parents[2]), [])


if __name__ == "__main__":
    unittest.main()
