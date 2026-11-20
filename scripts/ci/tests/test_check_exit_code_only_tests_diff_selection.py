#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exit-code-only checker tests for diff selection."""

from __future__ import annotations

import functools
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_exit_code_only_tests as checker  # noqa: E402
from scripts.ci.tests.test_check_exit_code_only_tests import (  # noqa: E402
    REL_TEST_FILE,
    UNRELATED_OVER_BOUND_COUNT,
    ExitCodeOnlyFixture,
)
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402


class ExitCodeOnlyDiffSelectionTest(ExitCodeOnlyFixture):
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


if __name__ == "__main__":
    unittest.main()
