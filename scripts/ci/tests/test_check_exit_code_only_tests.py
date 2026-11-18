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
from unittest import mock

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

    def test_hunk_above_def_decorator_or_excuse_selects_function(self) -> None:
        base = (
            "import subprocess\n"
            "# comment\n"
            "def test_w() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
            "    assert r.stdout == ''\n"
        )
        self._commit_file(REL_TEST_FILE, base, "base replay")
        replay = (
            "import subprocess\n"
            "# exit-code-only: short\n"
            "def test_w() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, replay, "short excuse and drop stdout assert")
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:3: test_w:", err)
        self.assertIn("reason must be at least 10 characters", err)

    def test_excuse_removed_or_decorator_edited_above_def_selects_function(self) -> None:
        base = (
            "import subprocess\n\n"
            "# exit-code-only: valid long excuse comment\n"
            "def test_excuse_removed() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n\n"
            "@deco_a\n"
            "def test_decorator_edited() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, base, "base with excuse and decorator")
        edited = base.replace("# exit-code-only: valid long excuse comment\n", "").replace(
            "@deco_a\n", "@deco_b\n"
        )
        self._commit_file(REL_TEST_FILE, edited, "remove excuse line and edit decorator")
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:3: test_excuse_removed: asserts only exit code 0", err)
        self.assertIn(f"{REL_TEST_FILE}:8: test_decorator_edited: asserts only exit code 0", err)

    def test_hunk_above_def_positive_and_boundary_outside_span(self) -> None:
        base = (
            "import subprocess\n"
            "# module comment line 2\n"
            "@deco\n"
            "def test_decorated_untouched() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, base, "base")
        boundary_edit = base.replace("# module comment line 2\n", "# edited module comment\n")
        self._commit_file(REL_TEST_FILE, boundary_edit, "edit comment above decorator")
        rc_b, out_b, err_b = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_b, 0, err_b)
        self.assertIn("0 exit-code-only test findings", out_b)
        valid_excuse = boundary_edit.replace(
            "# edited module comment\n",
            "# exit-code-only: valid excuse above decorator\n",
        )
        self._commit_file(REL_TEST_FILE, valid_excuse, "valid excuse above decorator")
        rc_p, out_p, err_p = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_p, 0, err_p)
        self.assertIn("0 exit-code-only test findings", out_p)

    def test_pure_deletion_at_func_end_matches_and_misses_later_function(self) -> None:
        base = (
            "import subprocess\n\n"
            "def test_first() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
            "    assert r.stdout == 'ok'\n"
            "    assert r.stderr == ''\n\n"
            "def test_second_untouched() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, base, "base two functions")
        still_strong = base.replace("    assert r.stderr == ''\n", "")
        self._commit_file(REL_TEST_FILE, still_strong, "delete last line of still-strong first")
        rc_ok, out_ok, err_ok = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_ok, 0, err_ok)
        self.assertIn("0 exit-code-only test findings", out_ok)
        now_weak = still_strong.replace("    assert r.stdout == 'ok'\n", "")
        self._commit_file(REL_TEST_FILE, now_weak, "delete final output assert of first")
        rc_fail, _out_f, err_fail = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_fail, 1)
        self.assertIn(f"{REL_TEST_FILE}:3: test_first: asserts only exit code 0", err_fail)
        self.assertNotIn("test_second_untouched", err_fail)

    def test_unchanged_rename_passes_and_partial_rename_flags_only_changed_function(self) -> None:
        old_rel = "scripts/ci/tests/test_old_name.py"
        new_rel = "scripts/ci/tests/test_new_name.py"
        base = (
            "import subprocess\n\n"
            "def test_untouched_in_rename() -> None:\n"
            "    res = subprocess.run(['true'], check=False)\n"
            "    assert res.returncode == 0\n\n"
            "def test_changed_in_rename() -> None:\n"
            "    res = subprocess.run(['true'], check=False)\n"
            "    assert res.returncode == 0\n"
            "    assert res.stdout == 'ok'\n"
        )
        self._commit_file(old_rel, base, "base before rename")
        self._git("mv", old_rel, new_rel)
        self._git("commit", "-q", "-m", "pure rename")
        rc_pure, out_pure, err_pure = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_pure, 0, err_pure)
        self.assertIn("0 exit-code-only test findings", out_pure)
        edited = base.replace("    assert res.stdout == 'ok'\n", "")
        self._commit_file(new_rel, edited, "drop stdout assert after rename")
        rc_edit, _out_e, err_edit = self._run_checker("--changed-against", "HEAD~2")
        self.assertEqual(rc_edit, 1)
        self.assertIn(f"{new_rel}:7: test_changed_in_rename: asserts only exit code 0", err_edit)
        self.assertNotIn("test_untouched_in_rename", err_edit)

    def test_working_tree_git_diff_head_failure_exits_2(self) -> None:
        base = (
            "import subprocess\n\n"
            "def test_committed_weak() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n\n"
            "def test_worktree_target() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
            "    assert r.stdout == 'ok'\n"
        )
        target = self._commit_file(REL_TEST_FILE, base, "base with committed weak test")
        rc_clean, out_clean, err_clean = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc_clean, 0, err_clean)
        self.assertIn("0 exit-code-only test findings", out_clean)
        target.write_text(base.replace("    assert r.stdout == 'ok'\n", ""), encoding="utf-8")
        rc_dirty, _out_d, err_dirty = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc_dirty, 1)
        self.assertIn("test_worktree_target: asserts only exit code 0", err_dirty)
        self.assertNotIn("test_committed_weak", err_dirty)
        (self.repo / ".git" / "index").write_bytes(b"corrupt-index")
        rc_err, _out_e, err_err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc_err, 2)
        self.assertIn(f"{self.repo}: git diff against HEAD failed:", err_err)

    def test_weak_boolean_and_no_assertion_forms_fail(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n\n"
                "def test_assert_not_rc() -> None:\n"
                "    proc = subprocess.run(['true'], check=False)\n"
                "    assert not proc.returncode\n\n"
                "def test_assert_false_proc_rc(self) -> None:\n"
                "    proc = subprocess.run(['true'], check=False)\n"
                "    self.assertFalse(proc.returncode)\n\n"
                "def test_assert_false_rc_var(self) -> None:\n"
                "    rc = _run_checker(['--ok'])\n"
                "    self.assertFalse(rc)\n\n"
                "def test_no_assertion() -> None:\n"
                "    subprocess.run(['true'], check=False)\n\n"
                "def test_only_check_true() -> None:\n"
                "    subprocess.run(['true'], check=True)\n\n"
                "def test_only_check_returncode() -> None:\n"
                "    proc = subprocess.run(['true'], check=False)\n"
                "    proc.check_returncode()\n\n"
                "def test_main_no_assertion() -> None:\n"
                "    main(['--ok'])\n\n"
                "def test_assert_not_with_output() -> None:\n"
                "    proc = subprocess.run(['true'], check=False)\n"
                "    assert not proc.returncode\n"
                "    assert proc.stdout == 'ok'\n"
            ),
            "weak boolean and no-assertion tests",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        for name in ("test_assert_not_rc", "test_assert_false_proc_rc", "test_assert_false_rc_var"):
            self.assertIn(f"{name}: asserts only exit code 0", err)
        for name in (
            "test_no_assertion",
            "test_only_check_true",
            "test_only_check_returncode",
            "test_main_no_assertion",
        ):
            self.assertIn(f"{name}: runs a command and asserts nothing", err)
        self.assertNotIn("test_assert_not_with_output", err)

    def test_assert_true_tuple_subscript_and_bool_literal_forms(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n\n"
                "def test_assert_true_rc_zero(self) -> None:\n"
                "    res = subprocess.run(['true'], check=False)\n"
                "    self.assertTrue(res.returncode == 0)\n\n"
                "def test_tuple_subscript_rc_zero() -> None:\n"
                "    res = _run_checker(['--ok'])\n"
                "    assert res[0] == 0\n\n"
                "def test_rc_eq_false_bool() -> None:\n"
                "    rc = _run_checker(['--ok'])\n"
                "    assert rc == False\n"
            ),
            "assertTrue, subscript, and bool literal tests",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn("test_assert_true_rc_zero: asserts only exit code 0", err)
        self.assertIn("test_tuple_subscript_rc_zero: asserts only exit code 0", err)
        self.assertNotIn("test_rc_eq_false_bool", err)

    def test_escape_reason_length_boundary_9_and_10_chars(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        nine_chars = (
            "import subprocess\n\n"
            "# exit-code-only: 123456789\n"
            "def test_nine_char_reason() -> None:\n"
            "    res = subprocess.run(['true'], check=False)\n"
            "    assert res.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, nine_chars, "9-char reason")
        rc9, _out9, err9 = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc9, 1)
        self.assertIn("test_nine_char_reason: # exit-code-only: reason must be at least 10", err9)
        ten_chars = nine_chars.replace("123456789", "1234567890")
        self._commit_file(REL_TEST_FILE, ten_chars, "10-char reason")
        rc10, out10, err10 = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc10, 0, err10)
        self.assertIn("0 exit-code-only test findings", out10)

    def test_file_above_one_mib_exits_2(self) -> None:
        target = self.repo / REL_TEST_FILE
        target.write_text("#" + ("a" * (checker.MAX_FILE_BYTES - 1)), encoding="utf-8")
        rc_ok, out_ok, err_ok = self._run_checker()
        self.assertEqual(rc_ok, 0, err_ok)
        self.assertIn("0 function(s) flagged", out_ok)
        target.write_text("#" + ("a" * checker.MAX_FILE_BYTES), encoding="utf-8")
        rc_big, _out_b, err_big = self._run_checker()
        self.assertEqual(rc_big, 2)
        self.assertIn(
            f"{REL_TEST_FILE}: exceeds {checker.MAX_FILE_BYTES} byte read limit",
            err_big,
        )

    def test_derived_output_values_pass_while_returncode_and_aliases_fail(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "def test_stdout_count_zero() -> None:\n"
                "    count = run_checker().stdout.count('ERROR')\n"
                "    assert count == 0\n\n"
                "def test_stdout_attr_and_len() -> None:\n"
                "    out = run_checker().stdout\n"
                "    err_len = len(run_checker().stderr)\n"
                "    assert len(out) == 0\n"
                "    assert err_len == 0\n\n"
                "def test_rc_named_var_bound_to_output() -> None:\n"
                "    status = run_checker().stdout.count('ERROR')\n"
                "    assert status == 0\n\n"
                "def test_tuple_index_one_output_boundary() -> None:\n"
                "    second = _run_checker(['--ok'])[1]\n"
                "    assert second == 0\n\n"
                "def test_tuple_index_zero_rc_boundary() -> None:\n"
                "    first = _run_checker(['--ok'])[0]\n"
                "    assert first == 0\n\n"
                "def test_chained_returncode_refused() -> None:\n"
                "    rc = run_checker().returncode\n"
                "    assert rc == 0\n\n"
                "def test_int_callee_alias_refused() -> None:\n"
                "    rc = main(['--ok'])\n"
                "    alias = rc\n"
                "    assert alias == 0\n"
            ),
            "derived output values vs return codes",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn("test_tuple_index_zero_rc_boundary: asserts only exit code 0", err)
        self.assertIn("test_chained_returncode_refused: asserts only exit code 0", err)
        self.assertIn("test_int_callee_alias_refused: asserts only exit code 0", err)
        for clean_name in (
            "test_stdout_count_zero",
            "test_stdout_attr_and_len",
            "test_rc_named_var_bound_to_output",
            "test_tuple_index_one_output_boundary",
        ):
            self.assertNotIn(clean_name, err)

    def test_color_always_git_config_still_reports_changed_findings(self) -> None:
        self._git("config", "color.ui", "always")
        self._git("config", "color.diff", "always")
        base = (
            "import subprocess\n\n"
            "def test_untouched_under_color() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n\n"
            "def test_boundary_under_color() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, base, "base under color.ui=always")
        changed = (
            base[:-1]
            + "  # boundary touch on final line\n\n"
            + "def test_weak_under_color() -> None:\n"
            + "    r = subprocess.run(['true'], check=False)\n"
            + "    assert r.returncode == 0\n\n"
            + "def test_strong_under_color() -> None:\n"
            + "    r = subprocess.run(['true'], check=False)\n"
            + "    assert r.returncode == 0\n"
            + "    assert r.stdout == 'ok'\n"
        )
        self._commit_file(REL_TEST_FILE, changed, "changes under color.ui=always")
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn("test_boundary_under_color: asserts only exit code 0", err)
        self.assertIn("test_weak_under_color: asserts only exit code 0", err)
        self.assertNotIn("test_untouched_under_color", err)
        self.assertNotIn("test_strong_under_color", err)

    def test_noprefix_mnemonic_ext_diff_and_renames_config_neutralised(self) -> None:
        self._git("config", "diff.noprefix", "true")
        self._git("config", "diff.mnemonicPrefix", "true")
        self._git("config", "diff.external", "/bin/false")
        self._git("config", "diff.renames", "false")
        other_old = "scripts/ci/tests/test_other_old.py"
        other_new = "scripts/ci/tests/test_other_new.py"
        weak_body = (
            "import subprocess\n\n"
            "def test_untouched_in_renamed() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(other_old, weak_body, "base rename source")
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base target")
        self._git("mv", other_old, other_new)
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n\n"
                "def test_prefix_weak() -> None:\n"
                "    r = subprocess.run(['true'], check=False)\n"
                "    assert r.returncode == 0\n\n"
                "def test_prefix_strong() -> None:\n"
                "    r = subprocess.run(['true'], check=False)\n"
                "    assert r.returncode == 0\n"
                "    assert r.stdout == 'ok'\n"
            ),
            "rename and edit under adversarial diff config",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn("test_prefix_weak: asserts only exit code 0", err)
        self.assertNotIn("test_prefix_strong", err)
        self.assertNotIn("test_untouched_in_renamed", err)

    def test_committed_change_with_uncommitted_lines_above_and_uncommitted_revert(self) -> None:
        base = (
            "import subprocess\n\n"
            "def test_a() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n\n"
            "def test_b() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, base, "base with a and b")
        committed_b = base.replace(
            "def test_b() -> None:\n    r = subprocess.run(['true'], check=False)\n",
            "def test_b() -> None:\n    r = subprocess.run(['true', '-v'], check=False)\n",
        )
        target = self._commit_file(REL_TEST_FILE, committed_b, "commit change to b")
        padding = "".join(f"# uncommitted line {i}\n" for i in range(12))
        target.write_text(padding + committed_b, encoding="utf-8")
        rc_shift, _out_s, err_shift = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_shift, 1)
        self.assertIn(f"{REL_TEST_FILE}:19: test_b: asserts only exit code 0", err_shift)
        self.assertNotIn("test_a", err_shift)
        target.write_text(base, encoding="utf-8")
        rc_rev, out_rev, err_rev = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_rev, 0, err_rev)
        self.assertIn("0 exit-code-only test findings in changed functions", out_rev)

    def test_failing_merge_base_and_bad_ref_exit_2_with_git_message(self) -> None:
        self._commit_file(REL_TEST_FILE, "def test_ok() -> None:\n    assert 1 == 1\n", "base")
        rc_bad, _out_b, err_bad = self._run_checker("--changed-against", "bad-ref-xyz")
        self.assertEqual(rc_bad, 2)
        self.assertIn("git merge-base", err_bad)
        self.assertIn("bad-ref-xyz", err_bad)
        blob_ref = f"HEAD:{REL_TEST_FILE}"
        rc_blob, _out_bl, err_blob = self._run_checker("--changed-against", blob_ref)
        self.assertEqual(rc_blob, 2)
        self.assertIn("git merge-base", err_blob)
        self.assertIn("blob", err_blob)
        self._git("branch", "first-root")
        self._git("checkout", "-q", "--orphan", "unrelated-root")
        self._commit_file(REL_TEST_FILE, "def test_ok() -> None:\n    assert 2 == 2\n", "orphan")
        rc_orph, _out_o, err_orph = self._run_checker("--changed-against", "first-root")
        self.assertEqual(rc_orph, 2)
        self.assertIn("git merge-base", err_orph)
        self.assertIn("no merge base found", err_orph)

    def test_single_git_diff_call_per_run_across_multiple_files(self) -> None:
        rel_a = "scripts/ci/tests/test_multi_a.py"
        rel_b = "scripts/ci/tests/test_multi_b.py"
        rel_c = "scripts/ci/tests/test_multi_c.py"
        rel_u = "scripts/ci/tests/test_multi_untracked.py"
        weak_stub = (
            "import subprocess\n\n"
            "def test_old_weak() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(rel_a, weak_stub, "base a")
        self._commit_file(rel_b, weak_stub, "base b")
        self._commit_file(rel_c, weak_stub, "base c")
        self._commit_file(
            rel_a,
            weak_stub
            + "\ndef test_added_weak_a() -> None:\n"
            + "    r = subprocess.run(['true'], check=False)\n"
            + "    assert r.returncode == 0\n",
            "edit a",
        )
        (self.repo / rel_b).write_text(
            weak_stub
            + "\ndef test_added_strong_b() -> None:\n"
            + "    r = subprocess.run(['true'], check=False)\n"
            + "    assert r.returncode == 0\n"
            + "    assert r.stdout == 'ok'\n",
            encoding="utf-8",
        )
        (self.repo / rel_u).write_text(
            "import subprocess\n\n"
            "def test_untracked_weak() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n",
            encoding="utf-8",
        )
        with mock.patch.object(checker, "_run_git", wraps=checker._run_git) as spy:
            rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        diff_calls = [c for c in spy.call_args_list if c.args[1][0] == "diff"]
        self.assertEqual(len(diff_calls), 1)
        self.assertEqual(rc, 1)
        self.assertIn("test_added_weak_a: asserts only exit code 0", err)
        self.assertIn("test_untracked_weak: asserts only exit code 0", err)
        self.assertNotIn("test_old_weak", err)
        self.assertNotIn("test_added_strong_b", err)

    def test_deletion_hunks_last_assert_whole_middle_function_and_line_zero(self) -> None:
        middle_block = (
            "\ndef test_middle() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n\n"
        )
        three_funcs = (
            "import subprocess\n\n"
            "def test_before() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
            "    assert r.stdout == 'ok'\n" + middle_block + "def test_after() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, three_funcs, "three functions")
        last_assert_deleted = three_funcs.replace("    assert r.stdout == 'ok'\n", "")
        self._commit_file(REL_TEST_FILE, last_assert_deleted, "delete last assert of test_before")
        rc_last, _out_l, err_last = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_last, 1)
        self.assertIn("test_before: asserts only exit code 0", err_last)
        self.assertNotIn("test_middle", err_last)
        for sep, ref in (("\n", "HEAD~1"), ("", "HEAD~2")):
            self._commit_file(
                REL_TEST_FILE, last_assert_deleted.replace(middle_block, sep), "delete middle"
            )
            rc_mid, out_mid, err_mid = self._run_checker("--changed-against", ref)
            self.assertEqual(rc_mid, 0, err_mid)
            self.assertIn("0 exit-code-only test findings in changed functions", out_mid)
        top_file = (
            "def test_deleted_at_zero() -> None:\n    assert 1 == 1\n\n"
            "# exit-code-only: valid excuse at top of file\n"
            "def test_top_surviving() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, top_file, "top file with function and excuse")
        top_func_deleted = top_file.split("\n\n", 1)[1]
        self._commit_file(REL_TEST_FILE, top_func_deleted, "delete top function at +0,0")
        rc_top0, out_top0, err_top0 = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_top0, 0, err_top0)
        self.assertIn("0 exit-code-only test findings in changed functions", out_top0)
        self._commit_file(REL_TEST_FILE, top_func_deleted.split("\n", 1)[1], "delete excuse +0,0")
        rc_exc0, _out_e0, err_exc0 = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_exc0, 1)
        self.assertIn(f"{REL_TEST_FILE}:1: test_top_surviving: asserts only exit code 0", err_exc0)


if __name__ == "__main__":
    unittest.main()
