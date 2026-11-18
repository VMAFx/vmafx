#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit and git-diff contract tests for scripts/ci/check_exit_code_only_tests.py."""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.ci import check_exit_code_only_tests as checker  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

GIT = shutil.which("git")
GIT_TIMEOUT_S = 30.0
REL_TEST_FILE = "scripts/ci/tests/test_sample.py"


class ExitCodeOnlyTestsCheckTest(unittest.TestCase):
    """Throwaway git repository tests for exit-code-only test detection."""

    def setUp(self) -> None:
        if GIT is None:
            self.skipTest("git executable is required for throwaway repository fixtures")
        self.git_bin: str = GIT
        self._tmp = tempfile.TemporaryDirectory(prefix="exit-code-only-test-")
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name)
        (self.repo / "scripts" / "ci" / "tests").mkdir(parents=True, exist_ok=True)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        self.env.update(
            GIT_CONFIG_GLOBAL=os.devnull,
            GIT_CONFIG_NOSYSTEM="1",
            GIT_AUTHOR_NAME="Test",
            GIT_AUTHOR_EMAIL="test@example.invalid",
            GIT_COMMITTER_NAME="Test",
            GIT_COMMITTER_EMAIL="test@example.invalid",
        )
        self._git("init", "-q")

    def _git(self, *args: str) -> None:
        run_command(
            [
                self.git_bin,
                "-C",
                str(self.repo),
                "-c",
                "commit.gpgsign=false",
                "-c",
                "core.hooksPath=/dev/null",
                *args,
            ],
            allowed_executables=(self.git_bin,),
            env=self.env,
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=GIT_TIMEOUT_S,
        )

    def _commit_file(self, rel_path: str, content: str, message: str = "test commit") -> Path:
        target = self.repo / rel_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
        self._git("add", "--", rel_path)
        self._git("commit", "-q", "-m", message)
        return target

    def _run_checker(self, *args: str) -> tuple[int, str, str]:
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = checker.main(["--root", str(self.repo), *args])
        return rc, out.getvalue(), err.getvalue()

    def test_planted_new_weak_test_fails_with_file_line_and_function(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n"
                "import unittest\n\n"
                "class SampleTest(unittest.TestCase):\n"
                "    def test_weak_returncode_only(self) -> None:\n"
                "        res = subprocess.run(['true'], check=False)\n"
                "        self.assertEqual(res.returncode, 0)\n"
            ),
            "add weak test",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:5: test_weak_returncode_only:", err)
        self.assertIn("asserts only exit code 0", err)

    def test_planted_helper_and_main_exit_code_only_forms_fail(self) -> None:
        self._commit_file(REL_TEST_FILE, "pass\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "def test_assert_rc_zero() -> None:\n"
                "    rc = _run_checker(['--ok'])\n"
                "    assert rc == 0\n\n"
                "def test_assert_main_zero(self) -> None:\n"
                "    self.assertEqual(main(['--ok']), 0)\n"
            ),
            "add helper and main weak tests",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:1: test_assert_rc_zero:", err)
        self.assertIn(f"{REL_TEST_FILE}:5: test_assert_main_zero:", err)

    def test_same_test_with_added_output_assertion_passes(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n"
                "import unittest\n\n"
                "class SampleTest(unittest.TestCase):\n"
                "    def test_checks_output_too(self) -> None:\n"
                "        res = subprocess.run(['true'], check=False, capture_output=True, text=True)\n"
                "        self.assertEqual(res.returncode, 0)\n"
                "        self.assertIn('tokens: 4', res.stdout)\n"
            ),
            "add strong test",
        )
        rc, out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        self.assertIn("0 exit-code-only test findings", out)

    def test_refusal_test_asserting_nonzero_exit_code_passes(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n"
                "import unittest\n\n"
                "class SampleTest(unittest.TestCase):\n"
                "    def test_refusal_path(self) -> None:\n"
                "        res = subprocess.run(['false'], check=False)\n"
                "        self.assertEqual(res.returncode, 1)\n"
            ),
            "add refusal test",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")

    def test_unchanged_old_weak_test_outside_diff_passes_changed_and_counts_default(self) -> None:
        base_code = (
            "import subprocess\n"
            "import unittest\n\n"
            "class SampleTest(unittest.TestCase):\n"
            "    def test_old_weak(self) -> None:\n"
            "        res = subprocess.run(['true'], check=False)\n"
            "        self.assertEqual(res.returncode, 0)\n"
        )
        self._commit_file(REL_TEST_FILE, base_code, "base with old weak test")
        added_strong = base_code + (
            "\n    def test_new_strong(self) -> None:\n"
            "        res = subprocess.run(['true'], check=False)\n"
            "        self.assertEqual(res.returncode, 0)\n"
            "        self.assertEqual(res.stdout, 'ok')\n"
        )
        self._commit_file(REL_TEST_FILE, added_strong, "add strong test below")
        rc_changed, _out_changed, err_changed = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_changed, 0, err_changed)
        self.assertEqual(err_changed, "")
        rc_default, out_default, err_default = self._run_checker()
        self.assertEqual(rc_default, 0, err_default)
        self.assertIn(f"{REL_TEST_FILE}:5: test_old_weak:", out_default)
        self.assertIn("1 function(s) flagged across 1 file(s)", out_default)

    def test_justified_escape_comment_passes(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n\n"
                "# exit-code-only: dry-run smoke check produces no output\n"
                "def test_escaped_above() -> None:\n"
                "    res = subprocess.run(['true'], check=False)\n"
                "    assert res.returncode == 0\n\n"
                "def test_escaped_inline() -> None:  # exit-code-only: silent no-op exit check\n"
                "    res = subprocess.run(['true'], check=False)\n"
                "    assert res.returncode == 0\n"
            ),
            "add escaped tests",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")

    def test_empty_escape_reason_fails(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n\n"
                "# exit-code-only:   \n"
                "def test_empty_escape() -> None:\n"
                "    res = subprocess.run(['true'], check=False)\n"
                "    assert res.returncode == 0\n"
            ),
            "add empty escape reason",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:4: test_empty_escape:", err)
        self.assertIn("reason must be at least 10 characters", err)

    def test_syntax_error_and_unreadable_file_exit_2(self) -> None:
        self._commit_file(REL_TEST_FILE, "def test_broken(:\n", "broken syntax")
        rc, _out, err = self._run_checker()
        self.assertEqual(rc, 2)
        self.assertIn(REL_TEST_FILE, err)
        self.assertIn("syntax error", err)
        rc_missing, _out_m, err_m = self._run_checker("scripts/ci/tests/test_missing.py")
        self.assertEqual(rc_missing, 2)
        self.assertIn("scripts/ci/tests/test_missing.py", err_m)

    def test_bad_ref_exits_2_with_path(self) -> None:
        self._commit_file(REL_TEST_FILE, "def test_ok() -> None:\n    assert 1 == 1\n", "base")
        rc, _out, err = self._run_checker("--changed-against", "nonexistent-ref-name")
        self.assertEqual(rc, 2)
        self.assertIn(str(self.repo), err)
        self.assertIn("nonexistent-ref-name", err)

    def test_boundary_function_span_touching_diff_by_one_line(self) -> None:
        base_lines = (
            "import subprocess\n"
            "import unittest\n\n"
            "class BoundaryTest(unittest.TestCase):\n"
            "    def test_span_boundary(self) -> None:\n"
            "        res = subprocess.run(['true'], check=False)\n"
            "        self.assertEqual(res.returncode, 0)\n"
            "# line 8 outside function span (5..7)\n"
        )
        self._commit_file(REL_TEST_FILE, base_lines, "base")
        outside_edit = base_lines.replace(
            "# line 8 outside function span (5..7)\n",
            "# line 8 edited just outside function span\n",
        )
        self._commit_file(REL_TEST_FILE, outside_edit, "edit line 8 outside span")
        rc_outside, _out_o, err_outside = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_outside, 0, err_outside)
        touching_last_line = outside_edit.replace(
            "        self.assertEqual(res.returncode, 0)\n",
            "        self.assertEqual(res.returncode, 0, 'touched line 7')\n",
        )
        self._commit_file(REL_TEST_FILE, touching_last_line, "touch line 7 (1-line overlap)")
        rc_touch, _out_t, err_touch = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_touch, 1)
        self.assertIn(f"{REL_TEST_FILE}:5: test_span_boundary:", err_touch)

    def test_subtest_block_counts_as_part_of_function(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n"
                "import unittest\n\n"
                "class SubTestSample(unittest.TestCase):\n"
                "    def test_subtest_weak(self) -> None:\n"
                "        with self.subTest(case='weak'):\n"
                "            res = subprocess.run(['true'], check=False)\n"
                "            self.assertEqual(res.returncode, 0)\n\n"
                "    def test_subtest_strong(self) -> None:\n"
                "        res = subprocess.run(['true'], check=False)\n"
                "        self.assertEqual(res.returncode, 0)\n"
                "        with self.subTest(case='output'):\n"
                "            self.assertEqual(res.stdout, 'expected')\n"
            ),
            "subtest cases",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:5: test_subtest_weak:", err)
        self.assertNotIn("test_subtest_strong", err)


if __name__ == "__main__":
    unittest.main()
