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
        path.write_text(yaml.safe_dump(workflow, sort_keys=False), encoding="utf-8", newline="\n")
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
        path.write_text(yaml.safe_dump(workflow, sort_keys=False), encoding="utf-8", newline="\n")
        return path

    def _write_env_workflow(self, name: str, env_value: str) -> Path:
        workflow = {
            "name": "Planted Env",
            "on": "push",
            "jobs": {
                "build": {
                    "runs-on": "ubuntu-26.04",
                    "env": {"PLANTED_VAL": env_value},
                    "steps": [{"name": "Noop", "run": "true"}],
                }
            },
        }
        path = self.tmp_path / name
        path.write_text(
            yaml.safe_dump(workflow, sort_keys=False, allow_unicode=True),
            encoding="utf-8",
            newline="\n",
        )
        return path

    def test_real_workflows_yml_pass_both_bounds(self) -> None:
        """Positive case: the real .github/workflows/*.yml files pass both checks."""
        yml_files = sorted((ROOT / ".github" / "workflows").glob("*.yml"))
        self.assertGreater(len(yml_files), 0)
        res = self.run_checker(*(str(path) for path in yml_files))
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_run_expression_length_boundary_21000_passes_and_21001_fails(self) -> None:
        """Boundary case: 21000-char runner expression passes and 21001-char fails."""
        # format('<prefix>{0}', github.sha) overhead after trimming ` github.sha ` is
        # 8 + 3 + 1 + 2 + 10 + 1 = 25 chars, so a 20975-char plain prefix gives 21000.
        exact_format = self._write_run_workflow(
            "exact_format.yml", "echo " + ("a" * 20970) + "${{ github.sha }}"
        )
        res_exact_format = self.run_checker(str(exact_format))
        self.assertEqual(res_exact_format.returncode, 0, res_exact_format.stderr)

        over_format = self._write_run_workflow(
            "over_format.yml", "echo " + ("a" * 20971) + "${{ github.sha }}"
        )
        res_over_format = self.run_checker(str(over_format))
        self.assertEqual(res_over_format.returncode, 1)
        self.assertIn("jobs.build.steps[0].run", res_over_format.stderr)
        self.assertIn("21001", res_over_format.stderr)

        # Single-expression scalar `${{ '<chars>' }}` trims to `'<chars>'` (length 1 + N + 1).
        exact_single = self._write_run_workflow(
            "exact_single.yml", "${{ '" + ("a" * 20998) + "' }}"
        )
        res_exact_single = self.run_checker(str(exact_single))
        self.assertEqual(res_exact_single.returncode, 0, res_exact_single.stderr)

        over_single = self._write_run_workflow("over_single.yml", "${{ '" + ("a" * 20999) + "' }}")
        res_over_single = self.run_checker(str(over_single))
        self.assertEqual(res_over_single.returncode, 1)
        self.assertIn("jobs.build.steps[0].run", res_over_single.stderr)
        self.assertIn("21001", res_over_single.stderr)

    def test_single_expression_whitespace_trim_boundary(self) -> None:
        """Boundary case: `${{<spaces>expr<spaces>}}` trims to 21000 passes, 21001 fails."""
        pad = " " * 50
        exact_inner = pad + "'" + ("a" * 20998) + "'" + pad
        self.assertGreater(len(exact_inner), 21000)
        exact_wf = self._write_run_workflow("trim_single_exact.yml", "${{" + exact_inner + "}}")
        res_exact = self.run_checker(str(exact_wf))
        self.assertEqual(res_exact.returncode, 0, res_exact.stderr)

        over_inner = pad + "'" + ("a" * 20999) + "'" + pad
        over_wf = self._write_run_workflow("trim_single_over.yml", "${{" + over_inner + "}}")
        res_over = self.run_checker(str(over_wf))
        self.assertEqual(res_over.returncode, 1)
        self.assertIn("jobs.build.steps[0].run", res_over.stderr)
        self.assertIn("21001", res_over.stderr)

    def test_format_expression_whitespace_trim_boundary(self) -> None:
        """Boundary case: literal + padded `${{ <expr> }}` trims to 21000 passes, 21001 fails."""
        padded_expr = "${{" + (" " * 50) + "github.sha" + (" " * 50) + "}}"
        exact_wf = self._write_run_workflow(
            "trim_format_exact.yml", "echo " + ("a" * 20970) + padded_expr
        )
        res_exact = self.run_checker(str(exact_wf))
        self.assertEqual(res_exact.returncode, 0, res_exact.stderr)

        over_wf = self._write_run_workflow(
            "trim_format_over.yml", "echo " + ("a" * 20971) + padded_expr
        )
        res_over = self.run_checker(str(over_wf))
        self.assertEqual(res_over.returncode, 1)
        self.assertIn("jobs.build.steps[0].run", res_over.stderr)
        self.assertIn("21001", res_over.stderr)

    def test_dotnet_whitespace_set_retains_u001f_and_trims_u3000(self) -> None:
        """Allow-list case: U+001F is retained in length while U+3000 is trimmed."""
        core_21000 = "'" + ("a" * 20998) + "'"
        u001f_padded = "${{" + ("\u001f" * 5) + core_21000 + ("\u001f" * 5) + "}}"
        wf_u001f = self._write_run_workflow("u001f_not_trimmed.yml", u001f_padded)
        res_u001f = self.run_checker(str(wf_u001f))
        self.assertEqual(res_u001f.returncode, 1)
        self.assertIn("jobs.build.steps[0].run", res_u001f.stderr)
        self.assertIn("21010", res_u001f.stderr)

        u3000_padded = "${{" + ("\u3000" * 5) + core_21000 + ("\u3000" * 5) + "}}"
        wf_u3000 = self._write_run_workflow("u3000_trimmed.yml", u3000_padded)
        res_u3000 = self.run_checker(str(wf_u3000))
        self.assertEqual(res_u3000.returncode, 0, res_u3000.stderr)

    def test_supplementary_emoji_counts_as_two_utf16_units(self) -> None:
        """Boundary case: supplementary U+1F600 counts 2 UTF-16 units (21000 passes, +1 fails)."""
        # format('<10487 emoji> {0}', github.sha) = 8 + (10487 * 2 + 1) + 3 + 1 + 12 + 1 = 21000.
        exact_env = ("\U0001f600" * 10487) + " ${{ github.sha }}"
        wf_exact = self._write_env_workflow("emoji_exact.yml", exact_env)
        res_exact = self.run_checker(str(wf_exact))
        self.assertEqual(res_exact.returncode, 0, res_exact.stderr)

        over_env = ("\U0001f600" * 10488) + " ${{ github.sha }}"
        self.assertLess(len(over_env), 21000)
        wf_over = self._write_env_workflow("emoji_over.yml", over_env)
        res_over = self.run_checker(str(wf_over))
        self.assertEqual(res_over.returncode, 1)
        self.assertIn("jobs.build.env.PLANTED_VAL", res_over.stderr)
        self.assertIn("21002", res_over.stderr)

        # Single-expression astral string literal: `'...emoji...'` = 1 + 10499 * 2 + 1 = 21000.
        wf_expr_exact = self._write_env_workflow(
            "emoji_expr_exact.yml", "${{ '" + ("\U0001f600" * 10499) + "' }}"
        )
        self.assertEqual(self.run_checker(str(wf_expr_exact)).returncode, 0)
        over_expr = "${{ '" + ("\U0001f600" * 10500) + "' }}"
        self.assertLess(len(over_expr), 21000)
        wf_expr_over = self._write_env_workflow("emoji_expr_over.yml", over_expr)
        self.assertEqual(self.run_checker(str(wf_expr_over)).returncode, 1)

    def test_bmp_non_ascii_literal_counts_as_one_utf16_unit(self) -> None:
        """Boundary case: BMP non-ASCII U+00E9 counts as 1 UTF-16 code unit, not 2 UTF-8 bytes."""
        exact_bmp = ("\u00e9" * 20975) + "${{ github.sha }}"
        wf_exact = self._write_env_workflow("bmp_exact.yml", exact_bmp)
        res_exact = self.run_checker(str(wf_exact))
        self.assertEqual(res_exact.returncode, 0, res_exact.stderr)

        over_bmp = ("\u00e9" * 20976) + "${{ github.sha }}"
        wf_over = self._write_env_workflow("bmp_over.yml", over_bmp)
        res_over = self.run_checker(str(wf_over))
        self.assertEqual(res_over.returncode, 1)
        self.assertIn("jobs.build.env.PLANTED_VAL", res_over.stderr)
        self.assertIn("21001", res_over.stderr)

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
