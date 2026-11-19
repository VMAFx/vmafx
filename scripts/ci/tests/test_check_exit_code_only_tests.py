#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit and git-diff contract tests for scripts/ci/check_exit_code_only_tests.py."""

from __future__ import annotations

import contextlib
import functools
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
from scripts.ci.tests.scanner_test_support import isolate_pre_commit_environment  # noqa: E402
from scripts.lib import new_scanner_findings  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

GIT = shutil.which("git")
GIT_TIMEOUT_S = 30.0
REL_TEST_FILE = "scripts/ci/tests/test_sample.py"
UNRELATED_OVER_BOUND_COUNT = 2_100


class ExitCodeOnlyTestsCheckTest(unittest.TestCase):
    """Throwaway git repository tests for exit-code-only test detection."""

    def setUp(self) -> None:
        if GIT is None:
            self.skipTest("git executable is required for throwaway repository fixtures")
        self.git_bin: str = GIT
        isolate_pre_commit_environment(self)
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

    def _git(self, *args: str) -> str:
        result = run_command(
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
        return result.stdout.strip()

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

        # Git env builders strip GIT_*; inject isolation at the subprocess boundary.
        def isolated_run(original, *args, **kwargs):
            kwargs["env"].update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
            return original(*args, **kwargs)

        with contextlib.ExitStack() as stack:
            stack.enter_context(contextlib.redirect_stdout(out))
            stack.enter_context(contextlib.redirect_stderr(err))
            for module in (checker, new_scanner_findings):
                original = module.run_command
                stack.enter_context(
                    mock.patch.object(
                        module, "run_command", functools.partial(isolated_run, original)
                    )
                )
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

    def test_added_separator_touches_old_weak_and_counts_default(self) -> None:
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
        self.assertEqual(rc_changed, 1, err_changed)
        self.assertIn("test_old_weak:", err_changed)
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

    def test_boundary_function_span_includes_trailing_comment(self) -> None:
        base_lines = (
            "import subprocess\n"
            "import unittest\n\n"
            "class BoundaryTest(unittest.TestCase):\n"
            "    def test_span_boundary(self) -> None:\n"
            "        res = subprocess.run(['true'], check=False)\n"
            "        self.assertEqual(res.returncode, 0)\n"
            "# line 8 after last statement\n"
        )
        self._commit_file(REL_TEST_FILE, base_lines, "base")
        outside_edit = base_lines.replace(
            "# line 8 after last statement\n",
            "# line 8 edited after last statement\n",
        )
        self._commit_file(REL_TEST_FILE, outside_edit, "edit line 8 outside span")
        rc_outside, _out_o, err_outside = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_outside, 1, err_outside)
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

    def test_decorator_edited_above_def_selects_function(self) -> None:
        base = (
            "import subprocess\n\n"
            "def test_untouched_above_decorator() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n\n"
            "@deco_a\n"
            "def test_decorator_edited() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, base, "base with decorator")
        edited = base.replace("@deco_a\n", "@deco_b\n")
        self._commit_file(REL_TEST_FILE, edited, "edit decorator")
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:8: test_decorator_edited: asserts only exit code 0", err)
        self.assertNotIn("test_untouched_above_decorator", err)

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
            "def test_modified_under_color() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, base, "base under color.ui=always")
        changed = (
            base.replace(
                "def test_modified_under_color() -> None:\n"
                "    r = subprocess.run(['true'], check=False)\n",
                "def test_modified_under_color() -> None:\n"
                "    r = subprocess.run(['true', '-v'], check=False)\n",
            )
            + "\ndef test_weak_under_color() -> None:\n"
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
        self.assertIn("test_modified_under_color: asserts only exit code 0", err)
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

    def test_hunk_diff_calls_per_changed_file(self) -> None:
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
        diff_calls = [c for c in spy.call_args_list if "diff" in c.args[1][:2]]
        self.assertEqual(len(diff_calls), 3)
        self.assertEqual(rc, 1)
        self.assertIn("test_added_weak_a: asserts only exit code 0", err)
        self.assertIn("test_untracked_weak: asserts only exit code 0", err)
        self.assertIn("test_old_weak", err)
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
            rc_mid, _out_mid, err_mid = self._run_checker("--changed-against", ref)
            self.assertEqual(rc_mid, 1, err_mid)
            self.assertIn("test_after:", err_mid)
        top_file = (
            "def test_deleted_at_zero() -> None:\n    assert 1 == 1\n\n"
            "def test_top_surviving() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, top_file, "top file with two functions")
        top_func_deleted = top_file.split("\n\n", 1)[1]
        self._commit_file(REL_TEST_FILE, top_func_deleted, "delete top function at +0,0")
        rc_top0, _out_top0, err_top0 = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc_top0, 1, err_top0)
        self.assertIn("test_top_surviving:", err_top0)

    def test_unrelated_files_over_max_test_files_do_not_hide_weak_test(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        aaa_dir = self.repo / "aaa"
        aaa_dir.mkdir(parents=True, exist_ok=True)
        for idx in range(UNRELATED_OVER_BOUND_COUNT):
            (aaa_dir / f"unrelated_{idx:04d}.txt").write_text("x\n", encoding="utf-8")
        self._git("add", "--", "aaa")
        (self.repo / REL_TEST_FILE).write_text(
            "import subprocess\n\n"
            "def test_weak_after_unrelated_flood() -> None:\n"
            "    rc = run_checker().returncode\n"
            "    assert rc == 0\n",
            encoding="utf-8",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(
            f"{REL_TEST_FILE}:3: test_weak_after_unrelated_flood: asserts only exit code 0",
            err,
        )

    def test_unrelated_large_diff_above_output_limit_is_ignored(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        snap = self._commit_file("testdata/snapshot.json", "{\n}\n", "base snapshot")
        snap.write_text("{\n" + ('  "score": 1,\n' * 1024) + "}\n", encoding="utf-8")
        self._git("add", "--", "testdata/snapshot.json")
        (self.repo / REL_TEST_FILE).write_text(
            "import subprocess\n\n"
            "def test_strong_with_large_unrelated_diff() -> None:\n"
            "    res = subprocess.run(['true'], check=False)\n"
            "    assert res.returncode == 0\n"
            "    assert res.stdout == 'ok'\n",
            encoding="utf-8",
        )
        small_limit_run = functools.partial(run_command, max_output_bytes=4096)
        with mock.patch.object(checker, "run_command", side_effect=small_limit_run):
            rc, out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        self.assertIn("0 exit-code-only test findings in changed functions", out)

    def test_over_bound_diff_records_and_unrelated_python_path(self) -> None:
        shell_a = self._commit_file("scripts/ci/tests/test-a.sh", "echo 1\n", "shell a")
        shell_b = self._commit_file("scripts/ci/tests/test-b.sh", "echo 1\n", "shell b")
        target = self._commit_file(
            REL_TEST_FILE,
            "def test_one() -> None:\n    assert 1 == 1\n",
            "base test",
        )
        shell_a.write_text("echo 2\n", encoding="utf-8")
        shell_b.write_text("echo 2\n", encoding="utf-8")
        target.write_text("def test_one() -> None:\n    assert 1 == 10\n", encoding="utf-8")
        with mock.patch.object(checker, "MAX_TEST_FILES", 2):
            rc_blk, _out_blk, err_blk = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc_blk, 2)
        self.assertIn("exceeds 2 file records", err_blk)
        helper_py = self.repo / "scripts/ci/tests/helper_not_a_test.py"
        helper_py.write_text("X = 1\n", encoding="utf-8")
        rc_py, _out_py, err_py = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc_py, 0, err_py)
        self.assertIn("0 exit-code-only", _out_py)

    def test_rename_from_outside_pathspec_into_tests_dir_checks_all_functions(self) -> None:
        outside_rel = "scripts/ci/legacy_moved_test.py"
        dest_rel = "scripts/ci/tests/test_moved_into_suite.py"
        self._commit_file(
            outside_rel,
            (
                "import subprocess\n\n"
                "def test_moved_weak() -> None:\n"
                "    res = subprocess.run(['true'], check=False)\n"
                "    assert res.returncode == 0\n\n"
                "def test_moved_strong() -> None:\n"
                "    res = subprocess.run(['true'], check=False)\n"
                "    assert res.returncode == 0\n"
                "    assert res.stdout == 'ok'\n"
            ),
            "base outside tests dir",
        )
        self._git("mv", outside_rel, dest_rel)
        self._git("commit", "-q", "-m", "move into scripts/ci/tests")
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn(f"{dest_rel}:3: test_moved_weak: asserts only exit code 0", err)
        self.assertNotIn("test_moved_strong", err)

    def test_umlaut_filename_with_and_without_quotepath_fails(self) -> None:
        self._commit_file(REL_TEST_FILE, "# init\n", "init")
        umlaut_rel = "scripts/ci/tests/test_ümlaut.py"
        weak_code = "def test_umlaut_weak() -> None:\n    rc = run_helper()\n    assert rc == 0\n"
        for quotepath in ("true", "false"):
            self._git("config", "core.quotepath", quotepath)
            (self.repo / umlaut_rel).write_text(weak_code, encoding="utf-8")
            rc_u, _out_u, err_u = self._run_checker("--changed-against", "HEAD")
            self.assertEqual(rc_u, 1)
            self.assertIn(f"{umlaut_rel}:1: test_umlaut_weak: asserts only exit code 0", err_u)
            (self.repo / umlaut_rel).unlink()
        target = self._commit_file(umlaut_rel, "# base\n", "base umlaut")
        target.write_text(weak_code, encoding="utf-8")
        for quotepath in ("true", "false"):
            self._git("config", "core.quotepath", quotepath)
            rc, _out, err = self._run_checker("--changed-against", "HEAD")
            self.assertEqual(rc, 1)
            self.assertIn(f"{umlaut_rel}:1: test_umlaut_weak: asserts only exit code 0", err)

    def test_filename_with_space_fails(self) -> None:
        space_rel = "scripts/ci/tests/test_with space.py"
        target = self._commit_file(space_rel, "# base\n", "base space")
        target.write_text(
            "def test_space_weak() -> None:\n    rc = run_helper()\n    assert rc == 0\n",
            encoding="utf-8",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(f"{space_rel}:1: test_space_weak: asserts only exit code 0", err)

    def test_explicit_path_absolute_and_parent_dotdot_normalised(self) -> None:
        exp_rel = "scripts/ci/tests/test_exp.py"
        target = self._commit_file(exp_rel, "# base\n", "base exp")
        target.write_text(
            "def test_exp_weak() -> None:\n    rc = run_helper()\n    assert rc == 0\n",
            encoding="utf-8",
        )
        for arg in (str(target), "scripts/ci/../ci/tests/test_exp.py"):
            rc, _out, err = self._run_checker("--changed-against", "HEAD", arg)
            self.assertEqual(rc, 1)
            self.assertIn(f"{exp_rel}:1: test_exp_weak: asserts only exit code 0", err)

    def test_removed_or_commented_trailing_assert_and_nested_def_fail(self) -> None:
        base_comment = (
            "def test_trailing_comment() -> None:\n"
            "    rc = run_helper()\n"
            "    assert rc == 0\n"
            "    # comment line\n"
            '    assert out == "ok"\n'
        )
        target = self._commit_file(REL_TEST_FILE, base_comment, "base with comment")
        for edited, expected in (
            (base_comment.replace('    assert out == "ok"\n', ""), 1),
            (base_comment.replace('    assert out == "ok"\n', '    # assert out == "ok"\n'), 1),
        ):
            target.write_text(edited, encoding="utf-8")
            rc, _out, err = self._run_checker("--changed-against", "HEAD")
            self.assertEqual(rc, expected, err)
        base_nested = (
            "def test_nested_removed() -> None:\n"
            "    rc = run_helper()\n"
            "    assert rc == 0\n"
            "    def local_check() -> str:\n"
            '        return "ok"\n'
            '    assert local_check() == "ok"\n'
        )
        target = self._commit_file(REL_TEST_FILE, base_nested, "base with nested def")
        target.write_text(
            "def test_nested_removed() -> None:\n    rc = run_helper()\n    assert rc == 0\n",
            encoding="utf-8",
        )
        rc_n, _out_n, err_n = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc_n, 1)
        self.assertIn(f"{REL_TEST_FILE}:1: test_nested_removed:", err_n)

    def test_path_outside_root_exits_2(self) -> None:
        self._commit_file(REL_TEST_FILE, "def test_ok() -> None:\n    assert 1 == 1\n", "base")
        with tempfile.NamedTemporaryFile(suffix=".py") as outside:
            for arg in (outside.name, "../outside_test.py"):
                rc, _out, err = self._run_checker("--changed-against", "HEAD", arg)
                self.assertEqual(rc, 2)
                self.assertIn("outside repository root", err)

    def test_record_parser_unknown_status_and_malformed_records_exit_2(self) -> None:
        for bad_record, expected in (
            ("C100\0a.py\0b.py\0", "C100"),
            ("U\0a.py\0", "U"),
            ("X\0a.py\0", "X"),
            ("M\0a.py", "not NUL-terminated"),
            ("R100\0a.py\0", "malformed git diff rename record"),
        ):
            with self.assertRaises(checker.ScanError) as ctx:
                checker._parse_name_status_z(bad_record)
            self.assertIn(expected, str(ctx.exception))
        parsed = checker._parse_name_status_z(
            "A\0a.py\0M\0m.py\0D\0d.py\0T\0t.py\0R090\0old.py\0new.py\0"
        )
        self.assertEqual(parsed, {"a.py": None, "m.py": "m.py", "t.py": None, "new.py": "old.py"})

    def test_comment_only_and_blank_line_edit_in_old_weak_test_reports(self) -> None:
        base = (
            "def test_old_weak() -> None:\n"
            "    rc = run_helper()\n"
            "    # existing comment\n"
            "    assert rc == 0\n"
        )
        target = self._commit_file(REL_TEST_FILE, base, "base old weak")
        target.write_text(
            "# header comment\n\n"
            "def test_old_weak() -> None:\n"
            "    rc = run_helper()\n"
            "    # edited comment and extra blank line below\n\n"
            "    assert rc == 0\n",
            encoding="utf-8",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1, err)
        self.assertIn("test_old_weak:", err)

    def _check_hunk_case(self, base: str, changed: str, expected: int) -> None:
        target = self._commit_file(REL_TEST_FILE, base)
        target.write_text(changed, encoding="utf-8")
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, expected, err)
        if expected == 1:
            self.assertIn("test_a:", err)

    def test_deleted_final_assertion_after_comments_is_reported(self) -> None:
        cases = (
            ("", "", "\ndef test_next():\n    assert True\n"),
            ("", "", ""),
            ("class Suite:\n", "    ", "\ndef after():\n    pass\n"),
            ("def helper():\n", "    ", "\ndef after():\n    pass\n"),
        )
        for prefix, indent, suffix in cases:
            with self.subTest(prefix=prefix, suffix=suffix):
                base = (
                    prefix
                    + indent
                    + "def test_a():\n"
                    + indent
                    + "    rc = run_helper()\n"
                    + indent
                    + "    assert rc == 0\n"
                    + indent
                    + "    # explanation\n\n"
                    + indent
                    + '    assert output == "ok"\n'
                    + suffix
                )
                self._check_hunk_case(
                    base, base.replace(indent + '    assert output == "ok"\n', ""), 1
                )

    def test_comment_between_tests_touches_upper_test(self) -> None:
        base = (
            "def test_a():\n    rc = run_helper()\n    assert rc == 0\n"
            "# old comment\n\ndef test_next():\n    assert True\n"
        )
        self._check_hunk_case(base, base.replace("old comment", "new comment"), 1)

    def test_enclosing_class_becomes_function(self) -> None:
        body = "    def test_a():\n        rc = run_helper()\n        assert rc == 0\n"
        self._check_hunk_case("class helper:\n" + body, "def helper():\n" + body, 1)

    def test_enclosing_function_becomes_async(self) -> None:
        body = "    def test_a():\n        rc = run_helper()\n        assert rc == 0\n"
        self._check_hunk_case("def helper():\n" + body, "async def helper():\n" + body, 1)

    def test_enclosing_function_becomes_class(self) -> None:
        body = "    def test_a():\n        rc = run_helper()\n        assert rc == 0\n"
        self._check_hunk_case("def helper():\n" + body, "class helper:\n" + body, 1)

    def test_new_weak_below_old_different_name(self) -> None:
        old = "def test_old():\n    rc = run_helper()\n    assert rc == 0\n"
        new = "\ndef test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        self._check_hunk_case(old, old + new, 1)

    def test_new_weak_uses_deleted_method_name(self) -> None:
        body = "    def test_a():\n        rc = run_helper()\n        assert rc == 0\n"
        self._check_hunk_case("class Old:\n" + body, "class New:\n" + body, 1)

    def test_move_weak_between_existing_helpers(self) -> None:
        body = "    def test_a():\n        rc = run_helper()\n        assert rc == 0\n"
        base = "def first():\n" + body + "\ndef second():\n    pass\n"
        changed = "def first():\n    pass\n\ndef second():\n" + body
        self._check_hunk_case(base, changed, 1)

    def test_same_name_weak_added_in_second_class(self) -> None:
        body = "    def test_a():\n        rc = run_helper()\n        assert rc == 0\n"
        base = "class Old:\n" + body
        self._check_hunk_case(base, base + "\nclass New:\n" + body, 1)

    def test_edit_immediately_above_decorator_does_not_touch_test(self) -> None:
        body = "@decorator\ndef test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        self._check_hunk_case("# old\n" + body, "# new\n" + body, 0)

    def test_pure_deletion_immediately_above_test_touches_test(self) -> None:
        body = "def test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        self._check_hunk_case("# remove\n" + body, body, 1)

    def test_enclosing_multiline_decorator_edit_touches_test(self) -> None:
        body = (
            "def helper():\n    def test_a():\n        rc = run_helper()\n        assert rc == 0\n"
        )
        self._check_hunk_case("@decorator(\n    1\n)\n" + body, "@decorator(\n    2\n)\n" + body, 1)

    def test_old_weak_untouched_while_sibling_changes(self) -> None:
        base = (
            "class Suite:\n"
            "    def test_old_weak(self) -> None:\n"
            "        rc = run_helper()\n"
            "        assert rc == 0\n\n"
            "    def test_sibling(self) -> None:\n"
            "        rc, out = run_helper()\n"
            "        assert rc == 0\n"
            '        assert out == "v1"\n'
        )
        target = self._commit_file(REL_TEST_FILE, base, "base sibling")
        target.write_text(base.replace('"v1"', '"v2"'), encoding="utf-8")
        rc_ok, out_ok, err_ok = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc_ok, 0, err_ok)
        self.assertIn("0 exit-code-only test findings in changed functions", out_ok)
        target.write_text(base.replace('        assert out == "v1"\n', ""), encoding="utf-8")
        rc_fail, _out_f, err_fail = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc_fail, 1)
        self.assertIn("test_sibling: asserts only exit code 0", err_fail)
        self.assertNotIn("test_old_weak", err_fail)

    def test_old_file_with_syntax_error_checks_all_functions(self) -> None:
        target = self._commit_file(REL_TEST_FILE, "def test_broken(:\n", "broken base")
        target.write_text(
            "def test_now_valid_weak() -> None:\n    rc = run_helper()\n    assert rc == 0\n\n"
            "def test_now_valid_strong() -> None:\n"
            "    rc, out = run_helper()\n"
            "    assert rc == 0\n"
            '    assert out == "ok"\n',
            encoding="utf-8",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:1: test_now_valid_weak: asserts only exit code 0", err)
        self.assertNotIn("test_now_valid_strong", err)

    def test_weak_top_level_moved_into_helper_is_reported(self) -> None:
        weak = "def test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, weak)
        target.write_text(
            "def helper():\n    def test_a():\n        rc = run_helper()\n        assert rc == 0\n",
            encoding="utf-8",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:2: test_a:", err)
        self.assertEqual(err.count(f"{REL_TEST_FILE}:"), 1)

    def test_unchanged_nested_weak_is_not_reported(self) -> None:
        base = (
            "def helper():\n    def test_a():\n        rc = run_helper()\n        assert rc == 0\n"
        )
        target = self._commit_file(REL_TEST_FILE, base)
        target.write_text("# unchanged test\n" + base, encoding="utf-8")
        rc, out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only test findings", out)
        self.assertNotIn("test_a:", err)

    def test_weak_moved_between_helpers_is_reported(self) -> None:
        base = "def helper_one():\n    def test_a():\n        rc = run_helper()\n        assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, base)
        target.write_text(base.replace("helper_one", "helper_two"), encoding="utf-8")
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:2: test_a:", err)
        self.assertEqual(err.count(f"{REL_TEST_FILE}:"), 1)

    def test_new_top_level_weak_matching_method_is_reported(self) -> None:
        base = (
            "class Suite:\n    def test_a():\n        rc = run_helper()\n        assert rc == 0\n"
        )
        target = self._commit_file(REL_TEST_FILE, base)
        target.write_text(
            base + "\ndef test_a():\n    rc = run_helper()\n    assert rc == 0\n", encoding="utf-8"
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:6: test_a:", err)
        self.assertIn(f"{REL_TEST_FILE}:2: test_a:", err)
        self.assertEqual(err.count(f"{REL_TEST_FILE}:"), 2)

    def test_weak_top_level_moved_into_async_helper_is_reported(self) -> None:
        weak = "def test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, weak)
        target.write_text(
            "async def helper():\n    def test_a():\n        rc = run_helper()\n        assert rc == 0\n",
            encoding="utf-8",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:2: test_a:", err)
        self.assertEqual(err.count(f"{REL_TEST_FILE}:"), 1)

    def test_duplicate_first_weakened_is_reported(self) -> None:
        strong = (
            'def test_a():\n    rc = run_helper()\n    assert rc == 0\n    assert output == "ok"\n'
        )
        weak = "def test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, strong + "\n" + weak)
        target.write_text(weak + "\n" + weak, encoding="utf-8")
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:1: test_a:", err)
        self.assertEqual(err.count(f"{REL_TEST_FILE}:"), 1)

    def test_duplicate_second_weakened_is_reported(self) -> None:
        strong = (
            'def test_a():\n    rc = run_helper()\n    assert rc == 0\n    assert output == "ok"\n'
        )
        weak = "def test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, strong + "\n" + strong)
        target.write_text(strong + "\n" + weak, encoding="utf-8")
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:6: test_a:", err)
        self.assertNotIn(f"{REL_TEST_FILE}:1: test_a:", err)

    def test_duplicates_unchanged_are_not_reported(self) -> None:
        strong = (
            'def test_a():\n    rc = run_helper()\n    assert rc == 0\n    assert output == "ok"\n'
        )
        weak = "def test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, strong + "\n" + weak)
        target.write_text("# comment\n" + strong + "\n" + weak, encoding="utf-8")
        rc, out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only test findings", out)
        self.assertNotIn("test_a:", err)

    def test_duplicate_added_weak_and_preceding_separator_are_reported(self) -> None:
        strong = (
            'def test_a():\n    rc = run_helper()\n    assert rc == 0\n    assert output == "ok"\n'
        )
        weak = "def test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, strong + "\n" + weak)
        target.write_text(strong + "\n" + weak + "\n" + weak, encoding="utf-8")
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:10: test_a:", err)
        self.assertIn(f"{REL_TEST_FILE}:6: test_a:", err)
        self.assertEqual(err.count(f"{REL_TEST_FILE}:"), 2)

    def test_duplicate_removal_touches_old_weak(self) -> None:
        strong = (
            'def test_a():\n    rc = run_helper()\n    assert rc == 0\n    assert output == "ok"\n'
        )
        weak = "def test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, strong + "\n" + weak)
        target.write_text(weak, encoding="utf-8")
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1, err)
        self.assertIn("test_a:", err)

    def test_duplicate_swap_touches_old_functions(self) -> None:
        strong = (
            'def test_a():\n    rc = run_helper()\n    assert rc == 0\n    assert output == "ok"\n'
        )
        weak = "def test_a():\n    rc = run_helper()\n    assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, strong + "\n" + weak)
        target.write_text(weak + "\n" + strong, encoding="utf-8")
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1, err)
        self.assertIn("test_a:", err)

    def test_duplicate_method_first_weakened_is_reported(self) -> None:
        strong = '    def test_a(self):\n        rc = run_helper()\n        assert rc == 0\n        assert output == "ok"\n'
        weak = "    def test_a(self):\n        rc = run_helper()\n        assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, "class Suite:\n" + strong + "\n" + weak)
        target.write_text("class Suite:\n" + weak + "\n" + weak, encoding="utf-8")
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:2: test_a:", err)
        self.assertEqual(err.count(f"{REL_TEST_FILE}:"), 1)

    def test_environment_over_bound_exits_2(self) -> None:
        self._commit_file(REL_TEST_FILE, "# base\n")
        with mock.patch.object(checker, "MAX_ENV_VARS", 0):
            rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 2)
        self.assertIn("environment exceeds 0 variables", err)

    def test_ast_slice_bounds_exit_2(self) -> None:
        self._commit_file(REL_TEST_FILE, "# base\n", "base")
        cases = (
            (
                "MAX_ASSIGN_TARGETS",
                1,
                "def test_x() -> None:\n    a, b = run_helper()\n    assert a == 0\n",
                "assignment target unpack exceeds 1 elements",
            ),
            (
                "MAX_ASSIGN_TARGETS",
                1,
                "def test_x() -> None:\n    a = b = run_helper()\n    assert a == 0\n",
                "assignment exceeds 1 targets",
            ),
            (
                "MAX_CALL_ARGS",
                1,
                "def test_x(self) -> None:\n"
                "    rc = run_helper()\n"
                "    self.assertEqual(first=rc, second=0)\n",
                "assertion call exceeds 1 keyword arguments",
            ),
            (
                "MAX_DECORATORS",
                1,
                "@d1\n@d2\ndef test_x() -> None:\n    rc = run_helper()\n    assert rc == 0\n",
                "definition exceeds 1 decorators",
            ),
        )
        for attr, limit, code, expected in cases:
            (self.repo / REL_TEST_FILE).write_text(code, encoding="utf-8")
            with mock.patch.object(checker, attr, limit):
                rc, _out, err = self._run_checker("--changed-against", "HEAD")
            self.assertEqual(rc, 2, f"{attr} did not exit 2: {err}")
            self.assertIn(expected, err)

    def test_awaited_returncodes_and_output(self) -> None:
        source = (
            "async def test_weak():\n    rc = await run_checker()\n    assert rc == 0\n"
            "async def test_attr():\n    assert (await run_checker()).returncode == 0\n"
            "async def test_output():\n    proc = await run_checker()\n"
            "    value = await read_output(proc.stdout)\n    assert value == 0\n"
        )
        found = checker.file_findings(source, REL_TEST_FILE)
        self.assertEqual([f.func_name for f in found], ["test_weak", "test_attr"])

    def test_reassignment_updates_assertion_binding(self) -> None:
        source = (
            "def test_output():\n    proc = run_checker()\n"
            "    value = proc.returncode\n    assert value == 0\n"
            "    value = len(proc.stdout)\n    assert value == 0\n"
        )
        self.assertEqual(checker.file_findings(source, REL_TEST_FILE), [])

    def test_escape_marker_requires_comment_token(self) -> None:
        source = (
            'def test_string(label="# exit-code-only: example marker"):\n'
            "    rc = run_checker()\n    assert rc == 0\n"
            "def test_comment():  # exit-code-only: example marker\n"
            "    rc = run_checker()\n    assert rc == 0\n"
        )
        self.assertEqual(
            [f.func_name for f in checker.file_findings(source, REL_TEST_FILE)], ["test_string"]
        )

    def test_inter_hunk_config_does_not_touch_old_weak_test(self) -> None:
        base = (
            "# before\n\n"
            + "def test_old():\n    rc = run_checker()\n    assert rc == 0\n"
            + "\ndef helper():\n    pass\n# after\n"
        )
        target = self._commit_file(REL_TEST_FILE, base)
        self._git("config", "diff.interHunkContext", "20")
        target.write_text(
            base.replace("# before", "# changed before").replace("# after", "# changed after"),
            encoding="utf-8",
        )
        rc, out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only test findings", out)

    def test_unicode_separator_in_diff_is_not_hunk_header(self) -> None:
        target = self._commit_file(REL_TEST_FILE, "# before\n")
        target.write_text("# changed\u2028@@ x\n", encoding="utf-8")
        rc, out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only test findings", out)

    def test_explicit_path_is_literal_git_pathspec(self) -> None:
        literal = "scripts/ci/tests/test_[1].py"
        other = "scripts/ci/tests/test_1.py"
        self._commit_file(literal, "# base\n")
        self._commit_file(other, "# base\n")
        (self.repo / literal).write_text("# changed\n", encoding="utf-8")
        (self.repo / other).write_text(
            "def test_weak():\n    rc = run_checker()\n    assert rc == 0\n", encoding="utf-8"
        )
        rc, out, err = self._run_checker("--changed-against", "HEAD", literal)
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only test findings", out)

    def test_changed_escape_marker_selects_weak_test(self) -> None:
        weak = "def test_x():\n    rc = run_checker()\n    assert rc == 0\n"
        for decorator in ("", "@decorator\n"):
            with self.subTest(decorator=decorator):
                target = self._commit_file(
                    REL_TEST_FILE, "# exit-code-only: silent status probe\n" + decorator + weak
                )
                target.write_text("# smoke test\n" + decorator + weak, encoding="utf-8")
                rc, _, err = self._run_checker("--changed-against", "HEAD")
                self.assertEqual(rc, 1, err)
                self.assertIn("test_x", err)

    def test_changed_unrelated_comment_does_not_select_weak_test(self) -> None:
        weak = "def test_x():\n    rc = run_checker()\n    assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, "# old\n# adjacent\n" + weak)
        target.write_text("# new\n# adjacent\n" + weak, encoding="utf-8")
        rc, out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only test findings", out)

    def test_changed_marker_deletion_and_short_addition(self) -> None:
        weak = "def test_x():\n    rc = run_checker()\n    assert rc == 0\n"
        marker = "# exit-code-only: silent status probe\n"
        target = self._commit_file(REL_TEST_FILE, marker + weak)
        for replacement in ("", "# exit-code-only: x\n"):
            with self.subTest(replacement=replacement):
                target.write_text(replacement + weak, encoding="utf-8")
                rc, _, err = self._run_checker("--changed-against", "HEAD")
                self.assertEqual(rc, 1, err)
                self.assertIn("test_x", err)
        self._commit_file(REL_TEST_FILE, weak)
        target.write_text("# exit-code-only: x\n" + weak, encoding="utf-8")
        rc, _, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1, err)
        self.assertIn("test_x", err)

    def test_changed_nonadjacent_marker_does_not_select_weak_test(self) -> None:
        weak = "def test_x():\n    rc = run_checker()\n    assert rc == 0\n"
        target = self._commit_file(
            REL_TEST_FILE, "# exit-code-only: silent status probe\n# old\n" + weak
        )
        target.write_text("# ordinary\n# new\n" + weak, encoding="utf-8")
        rc, out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only test findings", out)

    def test_outgoing_weak_commit_ignores_strong_worktree(self) -> None:
        strong = (
            "def test_x():\n    rc = run_checker()\n    assert rc == 0\n    assert output == 'ok'\n"
        )
        weak = strong.replace("    assert output == 'ok'\n", "")
        self._commit_file(REL_TEST_FILE, strong)
        self._commit_file(REL_TEST_FILE, weak)
        weak_ref = self._git("rev-parse", "HEAD")
        self._commit_file(REL_TEST_FILE, strong)
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": weak_ref}):
            rc, _, err = self._run_checker("--changed-against", "HEAD~2")
        self.assertEqual(rc, 1, err)
        self.assertIn("test_x", err)

    def test_outgoing_strong_commit_ignores_weak_worktree(self) -> None:
        strong = (
            "def test_x():\n    rc = run_checker()\n    assert rc == 0\n    assert output == 'ok'\n"
        )
        self._commit_file(REL_TEST_FILE, "# base\n")
        target = self._commit_file(REL_TEST_FILE, strong)
        target.write_text(strong.replace("    assert output == 'ok'\n", ""), encoding="utf-8")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "HEAD"}):
            rc, out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only test findings", out)

    def test_inline_escape_does_not_exempt_next_definition(self) -> None:
        source = (
            "def test_first(): pass  # exit-code-only: silent status probe\n"
            "def test_second():\n    rc = run_checker()\n    assert rc == 0\n"
        )
        self.assertEqual(
            [f.func_name for f in checker.file_findings(source, REL_TEST_FILE)], ["test_second"]
        )

    def test_branch_bindings_are_independent_and_merge(self) -> None:
        source = (
            "def test_branch(x):\n    proc = run_checker()\n    value = proc.returncode\n"
            "    if x:\n        value = len(proc.stdout)\n    else:\n        assert value == 0\n"
            "def test_straight():\n    proc = run_checker()\n    value = proc.returncode\n"
            "    value = len(proc.stdout)\n    assert value == 0\n"
        )
        self.assertEqual(
            [f.func_name for f in checker.file_findings(source, REL_TEST_FILE)], ["test_branch"]
        )

    def test_branch_join_and_compound_statements(self) -> None:
        prefix = "def test_case(x):\n    proc = run_checker()\n    value = proc.returncode\n"
        cases = (
            "    if x:\n        value = len(proc.stdout)\n    else:\n        value = len(proc.stdout)\n    assert value == 0\n",
            "    try:\n        value = len(proc.stdout)\n    except ValueError:\n        assert value == 0\n",
            "    match x:\n        case 1:\n            value = len(proc.stdout)\n        case _:\n            assert value == 0\n",
            "    for x in ():\n        value = len(proc.stdout)\n    else:\n        assert value == 0\n",
            "    while x:\n        value = len(proc.stdout)\n    else:\n        assert value == 0\n",
            "    with context():\n        value = len(proc.stdout)\n    assert value == 0\n",
        )
        for index, body in enumerate(cases):
            with self.subTest(case=index):
                findings = checker.file_findings(prefix + body, REL_TEST_FILE)
                self.assertEqual(bool(findings), index != 0)

    def test_compound_flow_carries_body_bindings(self) -> None:
        cases = (
            "    try:\n        rc = run_checker()\n    except ValueError:\n        pass\n    else:\n        assert rc == 0\n",
            "    for x in [1]:\n        rc = run_checker()\n    else:\n        assert rc == 0\n",
            "    async for x in items:\n        rc = run_checker()\n    else:\n        assert rc == 0\n",
            "    while x:\n        rc = run_checker()\n    else:\n        assert rc == 0\n",
            "    try:\n        rc = run_checker()\n    except ValueError:\n        pass\n    assert rc == 0\n",
            "    try:\n        rc = run_checker()\n    except ValueError:\n        assert rc == 0\n",
            "    for x in [1]:\n        rc = run_checker()\n        break\n    else:\n        rc = 5\n    assert rc == 0\n",
            "    try:\n        rc = run_checker()\n    except ValueError:\n        pass\n    finally:\n        assert rc == 0\n",
        )
        for index, body in enumerate(cases):
            with self.subTest(case=index):
                source = "async def test_case(x):\n" + body
                self.assertEqual(len(checker.file_findings(source, REL_TEST_FILE)), 1)

    def test_compound_paths_replace_bindings(self) -> None:
        cases = (
            "    try:\n        rc = 1\n    except ValueError:\n        rc = 2\n    assert rc == 0\n",
            "    if (rc := run_checker()):\n        pass\n    else:\n        assert rc == 0\n",
            "    while (rc := run_checker()):\n        break\n    else:\n        assert rc == 0\n",
        )
        for index, body in enumerate(cases):
            with self.subTest(case=index):
                prefix = (
                    "def test_case():\n    rc = run_checker()\n"
                    if index == 0
                    else "def test_case():\n"
                )
                self.assertEqual(
                    bool(checker.file_findings(prefix + body, REL_TEST_FILE)), index != 0
                )

    def test_insertions_displace_escape_marker(self) -> None:
        weak = "def test_old():\n    rc = run_checker()\n    assert rc == 0\n"
        marker = "# exit-code-only: silent status probe\n"
        target = self._commit_file(REL_TEST_FILE, marker + weak)
        insertions = (
            "\n",
            "# ordinary\n\n",
            "def test_new():\n    rc = run_checker()\n    assert rc == 0\n\n",
        )
        for insertion in insertions:
            with self.subTest(insertion=insertion):
                target.write_text(marker + insertion + weak, encoding="utf-8")
                rc, _, err = self._run_checker("--changed-against", "HEAD")
                self.assertEqual(rc, 1, err)
                self.assertIn("test_old", err)
                self.assertNotIn("test_new", err)

    def test_changes_without_marker_do_not_select_unchanged_test(self) -> None:
        weak = "def test_old():\n    rc = run_checker()\n    assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, "# first\n# second\n" + weak)
        for content in ("# second\n" + weak, "# first\n# second\n\n" + weak):
            with self.subTest(content=content):
                target.write_text(content, encoding="utf-8")
                rc, out, err = self._run_checker("--changed-against", "HEAD")
                self.assertEqual(rc, 0, err)
                self.assertIn("0 exit-code-only test findings", out)

    def test_changed_multiline_class_header_selects_method(self) -> None:
        source = "class TestX(\n    Base,\n):\n    def test_weak(self):\n        rc = run_checker()\n        assert rc == 0\n"
        target = self._commit_file(REL_TEST_FILE, source)
        target.write_text(source.replace("Base,", "Other,"), encoding="utf-8")
        rc, _, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1, err)
        self.assertIn("test_weak", err)

    def test_changed_support_module_is_ignored_but_bad_candidate_fails(self) -> None:
        self._commit_file(REL_TEST_FILE, "# test\n")
        support = self._commit_file("scripts/ci/tests/scanner_test_support.py", "# base\n")
        support.write_text("# changed\n", encoding="utf-8")
        rc, out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only", out)
        (self.repo / "scripts/ci/tests/test_bad.py").symlink_to("missing")
        rc, _, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 2, err)
        self.assertIn("test_bad.py", err)

    def test_outgoing_runner_failures_are_scan_errors(self) -> None:
        self._commit_file(REL_TEST_FILE, "# base\n")
        for attr in ("revision_modes", "revision_bytes"):
            for exc in (
                checker.CommandTimedOut(("git",), 1, None, None),
                checker.CommandOutputLimitExceeded(("git",), 1, None, None),
            ):
                with (
                    self.subTest(boundary=attr, error=type(exc).__name__),
                    mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "HEAD"}),
                    mock.patch.object(checker, attr, side_effect=exc),
                ):
                    rc, _, err = self._run_checker("--changed-against", "HEAD")
                    self.assertEqual(rc, 2, err)
                    self.assertIn("check_exit_code_only_tests: error:", err)
                    self.assertNotIn("Traceback", err)

    def test_checker_git_calls_isolate_ambient_global_config(self) -> None:
        self._commit_file(REL_TEST_FILE, "# base\n")
        (self.repo / REL_TEST_FILE).write_text(
            "def test_weak():\n    rc = run_checker()\n    assert rc == 0\n", encoding="utf-8"
        )
        original = checker.run_command

        def ambient_order_file(*args, **kwargs):
            env = kwargs["env"]
            if env.get("GIT_CONFIG_GLOBAL") != os.devnull or env.get("GIT_CONFIG_NOSYSTEM") != "1":
                raise OSError("global diff.orderFile missing in fixture")
            return original(*args, **kwargs)

        with mock.patch.object(checker, "run_command", side_effect=ambient_order_file):
            rc, _, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 1, err)
        self.assertIn("test_weak", err)

    def test_helper_assertion_requires_inline_output_assertion(self) -> None:
        docs = (ROOT / "docs/development/test-suites.md").read_text(encoding="utf-8")
        self.assertIn("assertions inside helpers such", docs)
        self.assertIn('assert out.stdout == "expected"', docs)
        weak = "def test_x():\n    out = run_checker()\n    assert out.returncode == 0\n    check_output(out)\n"
        self.assertEqual(len(checker.file_findings(weak, REL_TEST_FILE)), 1)
        self.assertEqual(
            checker.file_findings(weak + "    assert out.stdout == 'ok'\n", REL_TEST_FILE), []
        )


class ExitCodeOnlyHookEnvironmentTest(unittest.TestCase):
    def test_fixture_ignores_outer_hook_refs(self) -> None:
        """A changed-mode fixture test passes despite push refs absent from its repository."""
        injected = {
            "PRE_COMMIT_TO_REF": "f" * 40,
            "PRE_COMMIT_FROM_REF": "e" * 40,
        }
        output = io.StringIO()
        with mock.patch.dict(os.environ, injected):
            case = ExitCodeOnlyTestsCheckTest(
                "test_planted_new_weak_test_fails_with_file_line_and_function"
            )
            result = unittest.TextTestRunner(stream=output).run(unittest.TestSuite([case]))
            self.assertEqual(result.testsRun, 1)
            self.assertTrue(result.wasSuccessful(), output.getvalue())
            self.assertEqual({key: os.environ[key] for key in injected}, injected)


if __name__ == "__main__":
    unittest.main()
