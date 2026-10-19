# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for check_hosted_runner_labels.py (HISS-15)."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ci/check_hosted_runner_labels.py"
TODAY = "2026-10-11"

# The job ADR-1354 keeps on the older image; every fixture tree carries it, because the
# check also fails when that job stops naming the image.
SUPPLY_CHAIN = """\
name: Supply chain
on: push
jobs:
  verify-native-artifacts:
    runs-on: ubuntu-24.04
    steps:
      - run: echo ok
"""

GOOD = """\
name: Demo
on: push
jobs:
  build:
    # ubuntu-latest in a YAML comment is no runner choice.
    runs-on: ubuntu-26.04
    steps:
      - run: |
          # ubuntu-latest in a script comment is no runner choice either.
          sudo add-apt-repository ppa:ubuntu-toolchain-r/test
          docker pull rocm/dev-ubuntu-24.04:10.1.0-full
  arm:
    strategy:
      matrix:
        runner: [ubuntu-26.04, ubuntu-26.04-arm]
    runs-on: ${{ matrix.runner }}
    steps:
      - run: echo ok
  pool:
    runs-on: ${{ vars.ARC_RUNNERS_ENABLED == 'true' && 'arc-runners' || 'ubuntu-26.04' }}
    steps:
      - run: echo ok
"""

OLDER = GOOD.replace("    runs-on: ubuntu-26.04\n", "    runs-on: ubuntu-24.04\n")

FALLBACK = """\
[[exception]]
path = ".github/workflows/demo.yml"
job = "build"
label = "ubuntu-24.04"
reason = "fixture: the build fails on ubuntu-26.04"
expires = {expires}
"""


class HostedRunnerLabelTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.write(SUPPLY_CHAIN, "supply-chain")

    def write(self, text: str, name: str = "demo") -> None:
        path = self.root / ".github/workflows" / f"{name}.yml"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def declare(self, text: str) -> None:
        path = self.root / ".config/lint-exceptions.d/hosted-runner-label.toml"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_check(self, root: Path | None = None) -> subprocess.CompletedProcess[str]:
        argv = [sys.executable, str(SCRIPT)] + ([str(root)] if root else [])
        return subprocess.run(  # noqa: S603 -- fixed interpreter and script argv
            argv,
            capture_output=True,
            text=True,
            check=False,
            timeout=120,
            env={**os.environ, "LINT_EXCEPTIONS_TODAY": TODAY},
        )

    def assert_refused(self, needle: str) -> None:
        res = self.run_check(self.root)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn(needle, res.stderr)

    def assert_accepted(self) -> subprocess.CompletedProcess[str]:
        res = self.run_check(self.root)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn(" 0 finding(s)", res.stdout)
        return res

    # Positive.

    def test_repository_workflows_pass(self) -> None:
        res = self.run_check()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn(" 0 finding(s)", res.stdout)

    def test_explicit_labels_comments_and_lookalikes_pass(self) -> None:
        self.write(GOOD)
        res = self.assert_accepted()
        self.assertIn("4 naming a hosted Ubuntu image", res.stdout)

    def test_declared_fallback_passes(self) -> None:
        self.write(OLDER)
        self.declare(FALLBACK.format(expires="2026-11-30"))
        res = self.assert_accepted()
        self.assertIn("1 dated fallback(s)", res.stdout)

    def test_two_declared_fallbacks_in_one_workflow_pass(self) -> None:
        self.write(OLDER.replace("[ubuntu-26.04, ubuntu-26.04-arm]", "[ubuntu-24.04]"))
        one = FALLBACK.format(expires="2026-11-30")
        self.declare(one + one.replace('"build"', '"arm"'))
        res = self.assert_accepted()
        self.assertIn("2 dated fallback(s)", res.stdout)
        self.declare(one)
        self.assert_refused("job 'arm': names 'ubuntu-24.04' without a dated fallback")

    # Negative: floating labels, wherever a workflow can spell one.

    def test_floating_literal_refused(self) -> None:
        self.write(GOOD.replace("    runs-on: ubuntu-26.04\n", "    runs-on: ubuntu-latest\n"))
        self.assert_refused("demo.yml: job 'build': names the floating label 'ubuntu-latest'")

    def test_floating_matrix_value_refused(self) -> None:
        self.write(GOOD.replace("[ubuntu-26.04, ubuntu-26.04-arm]", "[ubuntu-latest]"))
        self.assert_refused("demo.yml: job 'arm': names the floating label 'ubuntu-latest'")

    def test_floating_expression_fallback_refused(self) -> None:
        self.write(GOOD.replace("|| 'ubuntu-26.04' }}", "|| 'ubuntu-latest' }}"))
        self.assert_refused("demo.yml: job 'pool': names the floating label 'ubuntu-latest'")

    def test_floating_label_in_printed_matrix_refused(self) -> None:
        printed = """      - run: echo 'matrix={"include":[{"runner":"ubuntu-latest"}]}' >> "$GITHUB_OUTPUT"\n"""
        self.write(GOOD.replace("      - run: echo ok\n", printed, 1))
        self.assert_refused("demo.yml: job 'arm': names the floating label 'ubuntu-latest'")

    def test_floating_label_outside_jobs_refused(self) -> None:
        self.write(GOOD.replace("on: push\n", "on: push\nenv:\n  RUNNER: ubuntu-latest\n"))
        self.assert_refused("demo.yml: job '(workflow)': names the floating label")

    def test_larger_and_slim_floating_labels_refused(self) -> None:
        for label in ("ubuntu-latest-4-cores", "ubuntu-slim"):
            with self.subTest(label=label):
                self.write(GOOD.replace("    runs-on: ubuntu-26.04\n", f"    runs-on: {label}\n"))
                self.assert_refused(f"names the floating label '{label}'")

    def test_floating_label_cannot_be_declared(self) -> None:
        self.write(GOOD.replace("    runs-on: ubuntu-26.04\n", "    runs-on: ubuntu-latest\n"))
        self.declare(FALLBACK.format(expires="2026-11-30").replace("ubuntu-24.04", "ubuntu-latest"))
        self.assert_refused("'ubuntu-latest' is not an older versioned image")
        self.assert_refused("names the floating label 'ubuntu-latest'")

    # Negative: older images.

    def test_older_image_without_declaration_refused(self) -> None:
        self.write(OLDER)
        self.assert_refused("job 'build': names 'ubuntu-24.04' without a dated fallback")

    def test_compatibility_target_covers_its_job_only(self) -> None:
        self.write(SUPPLY_CHAIN.replace("verify-native-artifacts", "other"), "other")
        self.assert_refused("other.yml: job 'other': names 'ubuntu-24.04' without")

    def test_stale_compatibility_target_refused(self) -> None:
        self.write(SUPPLY_CHAIN.replace("ubuntu-24.04", "ubuntu-26.04"), "supply-chain")
        self.assert_refused("job 'verify-native-artifacts' no longer names 'ubuntu-24.04'")

    def test_expired_fallback_refused(self) -> None:
        self.write(OLDER)
        self.declare(FALLBACK.format(expires="2026-10-10"))
        self.assert_refused("demo.yml job 'build': expired on 2026-10-10")
        self.assert_refused("job 'build': names 'ubuntu-24.04' without a dated fallback")

    def test_stale_fallback_refused(self) -> None:
        self.write(GOOD)
        self.declare(FALLBACK.format(expires="2026-11-30"))
        self.assert_refused("the job does not name 'ubuntu-24.04' (remove the entry)")

    def test_fallback_of_another_job_does_not_cover(self) -> None:
        self.write(OLDER)
        self.declare(FALLBACK.format(expires="2026-11-30").replace('"build"', '"arm"'))
        self.assert_refused("job 'build': names 'ubuntu-24.04' without a dated fallback")

    def test_malformed_fallbacks_refused(self) -> None:
        good = FALLBACK.format(expires="2026-11-30")
        cases = {
            "missing job": good.replace('job = "build"\n', ""),
            "`expires` is not a date": good.replace("2026-11-30", '"2026-11-30"'),
            "`path` is not a file of": good.replace(".github/workflows/demo.yml", "demo.yml"),
            "unreadable": good + "[[exception\n",
        }
        for needle, text in cases.items():
            with self.subTest(needle=needle):
                self.write(OLDER)
                self.declare(text)
                self.assert_refused(needle)

    def test_unreadable_workflow_refused(self) -> None:
        self.write("jobs: [not, a, mapping]\n")
        self.assert_refused("demo.yml: unreadable: workflow has no `jobs` mapping")
        self.write("name: [unclosed\n")
        self.assert_refused("demo.yml: unreadable")

    def test_tree_without_workflows_refused(self) -> None:
        with tempfile.TemporaryDirectory() as empty:
            res = self.run_check(Path(empty))
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("no workflow found", res.stderr)

    # Boundary: the read bounds. Passing one is a finding, never a silent cut.

    def test_workflow_count_bound(self) -> None:
        for index in range(510):
            self.write(GOOD, f"w{index:03d}")
        self.write(
            GOOD.replace("    runs-on: ubuntu-26.04\n", "    runs-on: ubuntu-latest\n"), "zz"
        )
        # 512 files: the last one in sort order is still read.
        self.assert_refused("zz.yml: job 'build': names the floating label 'ubuntu-latest'")
        self.write(GOOD, "zzz")
        self.assert_refused("more than 512 workflows, raise the bound")

    def test_fallback_count_bound(self) -> None:
        self.write(OLDER)
        entry = FALLBACK.format(expires="2026-11-30")
        self.declare(entry * 256)
        res = self.run_check(self.root)
        self.assertNotIn("raise the bound", res.stderr)
        self.declare(entry * 257)
        self.assert_refused("more than 256 entries, raise the bound")

    def test_node_bound(self) -> None:
        # One list of 640 strings, named once and referenced 311 times: 312 * 641 nodes,
        # and 8 for the job's own mappings, keys and lists. That is 200 000 exactly; one
        # more string in the outer list passes the bound.
        head = "name: Demo\non: push\njobs:\n  build:\n    runs-on: ubuntu-26.04\n"
        items = ", ".join(["x"] * 640)
        refs = ", ".join(["*a"] * 311)
        for extra, refused in (("", False), (", y", True)):
            with self.subTest(extra=extra):
                self.write(f"{head}    env:\n      A: &a [{items}]\n      B: [{refs}{extra}]\n")
                res = self.run_check(self.root)
                self.assertEqual("raise the bound" in res.stderr, refused, res.stderr[-400:])
                self.assertEqual(res.returncode, 1 if refused else 0, res.stderr[-400:])

    # Boundary: the end date.

    def test_fallback_ending_today_is_live(self) -> None:
        self.write(OLDER)
        self.declare(FALLBACK.format(expires=TODAY))
        self.assert_accepted()

    def test_fallback_ninety_days_out_is_the_limit(self) -> None:
        self.write(OLDER)
        self.declare(FALLBACK.format(expires="2027-01-09"))
        self.assert_accepted()
        self.declare(FALLBACK.format(expires="2027-01-10"))
        self.assert_refused("ends 2027-01-10, more than 90 days out")


if __name__ == "__main__":
    unittest.main()
