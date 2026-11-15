#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for check-workflow-size.py: positive, negative and boundary cases (HISS-15)."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_CHECKER = ROOT / "scripts" / "ci" / "check-workflow-size.py"
CHECKER = Path(os.environ.get("WORKFLOW_SIZE_CHECKER_UNDER_TEST", DEFAULT_CHECKER))
SCRIPT_TIMEOUT_S = 60


class CheckWorkflowSizeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.tmp_path = Path(self.tmp.name)

    def run_checker(self, *args: str) -> subprocess.CompletedProcess[str]:
        """Run scripts/ci/check-workflow-size.py with the given arguments.

        Runs the workflow byte-size checker across positive, negative, and boundary files.
        """
        return subprocess.run(  # noqa: S603
            [sys.executable, str(CHECKER), *args],
            capture_output=True,
            text=True,
            check=False,
            timeout=SCRIPT_TIMEOUT_S,
        )

    def test_planted_oversize_file_fails(self) -> None:
        """Negative case: a 65537-byte file exceeds the 65536 bound and fails."""
        oversize = self.tmp_path / "oversize.yml"
        oversize.write_bytes(b"a" * 65537)
        res = self.run_checker(str(oversize))
        self.assertEqual(res.returncode, 1)
        self.assertIn("oversize.yml (65537 bytes) exceeds 65536 byte limit", res.stderr)

    def test_boundary_exact_size_passes(self) -> None:
        """Boundary case: a file of exactly 65536 bytes passes."""
        boundary = self.tmp_path / "boundary.yml"
        boundary.write_bytes(b"b" * 65536)
        res = self.run_checker(str(boundary))
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_boundary_one_byte_under_passes(self) -> None:
        """Boundary case: a file of 65535 bytes passes."""
        under = self.tmp_path / "under.yml"
        under.write_bytes(b"c" * 65535)
        res = self.run_checker(str(under))
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_repository_workflows_positive(self) -> None:
        """Positive case: all workflow files in the repository pass."""
        res = self.run_checker()
        self.assertEqual(res.returncode, 0, res.stderr)

    def _write_run_workflow(self, name: str, run_value: str) -> Path:
        workflow = {
            "name": "Planted Run",
            "on": "push",
            "jobs": {
                "build": {
                    "runs-on": "ubuntu-26.04",
                    "steps": [{"name": "Run step", "run": run_value}],
                }
            },
        }
        path = self.tmp_path / name
        path.write_text(yaml.safe_dump(workflow, sort_keys=False), encoding="utf-8")
        return path

    def _write_script_workflow(self, name: str, script_value: str) -> Path:
        workflow = {
            "name": "Planted Script",
            "on": "pull_request",
            "jobs": {
                "aggregator": {
                    "runs-on": "ubuntu-26.04",
                    "steps": [
                        {
                            "name": "Script step",
                            "uses": "actions/github-script@v9",
                            "with": {"script": script_value},
                        }
                    ],
                }
            },
        }
        path = self.tmp_path / name
        path.write_text(yaml.safe_dump(workflow, sort_keys=False), encoding="utf-8")
        return path

    def test_real_workflows_yml_pass_both_bounds(self) -> None:
        """Positive case: the real .github/workflows/*.yml files pass both checks."""
        yml_files = sorted((ROOT / ".github" / "workflows").glob("*.yml"))
        self.assertGreater(len(yml_files), 0)
        res = self.run_checker(*(str(path) for path in yml_files))
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_run_expression_length_boundary_21000_passes_and_21001_fails(self) -> None:
        """Boundary case: 21000-char runner expression passes and 21001-char fails."""
        # format('<prefix>{0}',  github.sha ) overhead is 8 + 3 + 1 + 2 + 12 + 1 = 27 chars,
        # so a 20973-char plain prefix gives exact expression length 21000.
        exact_format = self._write_run_workflow(
            "exact_format.yml", "echo " + ("a" * 20968) + "${{ github.sha }}"
        )
        res_exact_format = self.run_checker(str(exact_format))
        self.assertEqual(res_exact_format.returncode, 0, res_exact_format.stderr)

        over_format = self._write_run_workflow(
            "over_format.yml", "echo " + ("a" * 20969) + "${{ github.sha }}"
        )
        res_over_format = self.run_checker(str(over_format))
        self.assertEqual(res_over_format.returncode, 1)
        self.assertIn("jobs.build.steps[0].run", res_over_format.stderr)
        self.assertIn("21001", res_over_format.stderr)

        # Single-expression scalar `${{ '<chars>' }}` has inner length 2 + N + 2.
        exact_single = self._write_run_workflow(
            "exact_single.yml", "${{ '" + ("a" * 20996) + "' }}"
        )
        res_exact_single = self.run_checker(str(exact_single))
        self.assertEqual(res_exact_single.returncode, 0, res_exact_single.stderr)

        over_single = self._write_run_workflow("over_single.yml", "${{ '" + ("a" * 20997) + "' }}")
        res_over_single = self.run_checker(str(over_single))
        self.assertEqual(res_over_single.returncode, 1)
        self.assertIn("jobs.build.steps[0].run", res_over_single.stderr)
        self.assertIn("21001", res_over_single.stderr)

    def test_planted_30000_char_script_without_expression_passes(self) -> None:
        """Positive boundary: a 30000-char literal script with no `${{` has no 21000 bound."""
        base_script = ("const ok = 1;\n" * 2143)[:30000]
        self.assertEqual(len(base_script), 30000)
        self.assertNotIn("${{", base_script)
        workflow = self._write_script_workflow("literal_30k.yml", base_script)
        res = self.run_checker(str(workflow))
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_planted_30000_char_script_with_comment_expression_fails(self) -> None:
        """Negative case (r4 defect class): 30000-char script with one `${{` comment fails."""
        comment = "// stamp: [${{ github.sha }}]\n"
        base_script = ("const ok = 1;\n" * 2143)[: 30000 - len(comment)]
        planted_script = comment + base_script
        self.assertEqual(len(planted_script), 30000)
        workflow = self._write_script_workflow("comment_expr_30k.yml", planted_script)
        res = self.run_checker(str(workflow))
        self.assertEqual(res.returncode, 1)
        self.assertIn("steps[0].with.script", res.stderr)
        self.assertIn("21000", res.stderr)

    def test_escaped_braces_and_quotes_count_double(self) -> None:
        """Negative escape case: `{`, `}`, and `'` count double in `format(...)` literals."""
        # 20900 `{` chars + `${{ github.sha }}` is 20917 raw chars (< 21000),
        # and even unescaped format overhead would be 20927 (<= 21000),
        # while escaped `{` doubling yields 41827 (> 21000).
        raw_under_21k = ("{" * 20900) + "${{ github.sha }}"
        self.assertLess(len(raw_under_21k), 21000)
        brace_file = self._write_run_workflow("escaped_open_braces.yml", raw_under_21k)
        res_brace = self.run_checker(str(brace_file))
        self.assertEqual(res_brace.returncode, 1)
        self.assertIn("jobs.build.steps[0].run", res_brace.stderr)

        for label, ch in (("close_brace", "}"), ("single_quote", "'")):
            with self.subTest(escaped=label):
                planted = (ch * 10500) + "${{ github.sha }}"
                self.assertLess(len(planted), 21000)
                wf = self._write_run_workflow(f"escaped_{label}.yml", planted)
                res = self.run_checker(str(wf))
                self.assertEqual(res.returncode, 1)
                self.assertIn("jobs.build.steps[0].run", res.stderr)

    def test_unclosed_expression_fails(self) -> None:
        """Negative case: an unclosed `${{` (plain or inside single quotes) fails."""
        for label, bad_run in (
            ("missing_close", "echo ${{ github.sha"),
            ("close_inside_single_quotes", "echo ${{ '}}still_in_string"),
        ):
            with self.subTest(case=label):
                wf = self._write_run_workflow(f"unclosed_{label}.yml", bad_run)
                res = self.run_checker(str(wf))
                self.assertEqual(res.returncode, 1)
                self.assertIn("jobs.build.steps[0].run", res.stderr)
                self.assertIn("unclosed", res.stderr)


if __name__ == "__main__":
    unittest.main()
