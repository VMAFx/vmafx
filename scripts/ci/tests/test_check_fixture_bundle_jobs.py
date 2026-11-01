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
BOTH = 'SUPPORTED_BUNDLES=("pq-sollevante-v1" "hdr-ictcp-pixels-v1")\n'
GAP = "before the step, under its condition"


def workflow(steps: str) -> str:
    """A one-job workflow with the given step lines."""
    return "on: push\njobs:\n  t:\n    runs-on: ubuntu-latest\n    steps:\n" + steps


class FixtureBundleJobs(unittest.TestCase):
    def _check(
        self, steps: str, scripts: dict[str, str] | None = None, supported: str = BOTH
    ) -> list[str]:
        """Run the check on a temporary tree with one test file, one workflow, a fetch script."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "python" / "test").mkdir(parents=True)
            (root / "python" / "test" / "hdr_pin_test.py").write_text(TEST_FILE)
            (root / "python" / "test" / "plain_test.py").write_text("X = ('yuv', 'a.yuv')\n")
            (root / ".github" / "workflows").mkdir(parents=True)
            (root / ".github" / "workflows" / "w.yml").write_text(workflow(steps))
            (root / "scripts" / "test").mkdir(parents=True)
            (root / MOD.FETCH_SCRIPT).write_text(supported)
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
        self.assertIn(f"hdr-ictcp-pixels-v1 (hdr_pin_test) {GAP}", problems[0])
        self.assertIn(f"pq-sollevante-v1 (hdr_pin_test) {GAP}", problems[1])

    def test_tox_without_one_bundle_fails(self) -> None:
        """Negative: tox runs the whole suite; fetching only the default bundle misses one."""
        problems = self._check(FETCH + "      - run: tox -c python\n")
        self.assertEqual(
            problems, [f"w.yml:t: does not fetch hdr-ictcp-pixels-v1 (hdr_pin_test) {GAP}"]
        )

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
            "      - if: always()\n        uses: ./.github/actions/pq-fixture\n"
            "        with:\n          phase: finish\n"
        )
        problems = self._check(finish + "      - run: tox -c python\n")
        self.assertEqual(len(problems), 2)

    def test_finish_without_a_failure_condition_fails(self) -> None:
        """Negative: a finish step with a plain condition is skipped after a red test."""
        steps = (
            FETCH
            + "      - run: tox -c python\n"
            + "      - name: Put away\n        if: matrix.os == 'macos-latest'\n"
            + FETCH.replace("phase: get", "phase: finish").replace("      - uses", "        uses")
        )
        problems = self._check(steps.replace(FETCH, FETCH + FETCH_ICTCP, 1))
        self.assertEqual(len(problems), 1)
        self.assertIn("'Put away' is skipped after a failed step", problems[0])

    def test_finish_with_no_condition_fails(self) -> None:
        """Negative: a finish step without any if: is skipped after a red test too."""
        finish = FETCH.replace("phase: get", "phase: finish")
        problems = self._check(FETCH + FETCH_ICTCP + "      - run: tox -c python\n" + finish)
        self.assertEqual(
            problems,
            ["w.yml:t: 'step 4' is skipped after a failed step; start its if: with !cancelled()"],
        )

    def test_finish_after_failure_passes(self) -> None:
        """Boundary: !cancelled() in an expression block and bare always() both run after a failure."""
        for cond in ("${{ !cancelled() && matrix.os == 'x' }}", "always()"):
            finish = (
                f"      - if: {cond}\n        uses: ./.github/actions/pq-fixture\n"
                "        with:\n          phase: finish\n"
            )
            self.assertEqual(
                self._check(FETCH + FETCH_ICTCP + "      - run: tox -c python\n" + finish), []
            )

    def test_fetch_after_the_test_fails(self) -> None:
        """Negative: a get step after the test step does not count for it."""
        problems = self._check("      - run: tox -c python\n" + FETCH + FETCH_ICTCP)
        self.assertEqual(len(problems), 2)

    def test_fetch_under_a_narrower_condition_fails(self) -> None:
        """Negative: a fetch that skips cases the test step runs in is a gap."""
        guarded = FETCH.replace("      - uses", "      - if: matrix.os == 'x'\n        uses")
        run = "      - if: matrix.os == 'x' || matrix.os == 'y'\n        run: tox -c python\n"
        problems = self._check(guarded + guarded.replace(FETCH, FETCH_ICTCP) + run)
        self.assertEqual(len(problems), 2)

    def test_fetch_under_a_wider_condition_passes(self) -> None:
        """Boundary: a fetch whose if: covers the test step's if: is enough."""
        cond = "      - if: (matrix.a && !matrix.b) || matrix.os == 'm'\n        uses"
        guarded = FETCH.replace("      - uses", cond) + FETCH_ICTCP.replace("      - uses", cond)
        run = "      - if: ${{ matrix.a && !matrix.b }}\n        run: tox -c python\n"
        self.assertEqual(self._check(guarded + run), [])

    def test_unsupported_bundle_fails(self) -> None:
        """Negative: a get step naming a bundle the fetch script does not list is refused."""
        run = "      - run: tox -c python\n"
        problems = self._check(
            FETCH + FETCH_ICTCP + run, supported='SUPPORTED_BUNDLES=("pq-sollevante-v1")\n'
        )
        self.assertIn(
            "fetches hdr-ictcp-pixels-v1, which scripts/test/fetch-test-yuvs.sh does not support",
            problems[0],
        )

    def test_missing_supported_line_fails(self) -> None:
        """Negative: without a SUPPORTED_BUNDLES line the check refuses to guess."""
        problems = self._check(FETCH, supported="BUNDLE_NAME=x\n")
        self.assertEqual(len(problems), 1)
        self.assertIn("no SUPPORTED_BUNDLES line", problems[0])

    def test_implies(self) -> None:
        """Boundary: implication over atoms, with function calls and comparisons as atoms."""
        self.assertTrue(MOD.implies("a && b", "(a && b) || c"))
        self.assertTrue(MOD.implies("x", ""))
        self.assertTrue(MOD.implies("${{ !cancelled() && x }}", "x"))
        self.assertFalse(MOD.implies("", "a"))
        self.assertFalse(MOD.implies("a || c", "a"))
        self.assertFalse(MOD.implies("a", "a && (b"))
        self.assertTrue(
            MOD.implies(
                "startsWith(matrix.os, 'u') && !s", "(startsWith(matrix.os, 'u') && !s) || m == 'k'"
            )
        )

    def test_repository_workflows_fetch_their_bundles(self) -> None:
        """The repository's own workflows have no gap."""
        self.assertEqual(MOD.check(SCRIPT.parents[2]), [])


if __name__ == "__main__":
    unittest.main()
