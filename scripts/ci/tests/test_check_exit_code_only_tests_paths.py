#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exit-code-only checker tests for paths."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_exit_code_only_tests as checker  # noqa: E402
from scripts.ci.tests.test_check_exit_code_only_tests import (  # noqa: E402
    REL_TEST_FILE,
    ExitCodeOnlyFixture,
)


class ExitCodeOnlyPathsTest(ExitCodeOnlyFixture):
    def test_r11_f9_default_symlink_outside(self) -> None:
        with tempfile.TemporaryDirectory(prefix="outside-test-") as directory:
            outside = Path(directory) / "outside.py"
            outside.write_text("def test_x():\n    assert True\n", encoding="utf-8")
            link = self.repo / "scripts/ci/tests/test_link.py"
            try:
                link.symlink_to(outside)
            except OSError as exc:
                self.skipTest(f"symlink unavailable: {exc}")
            rc, _, err = self._run_checker()
            self.assertEqual(rc, 2, err)
            self.assertIn("path is outside repository root", err)

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


if __name__ == "__main__":
    unittest.main()
