#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exit-code-only checker tests for markers."""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_exit_code_only_tests as checker  # noqa: E402
from scripts.ci.tests.test_check_exit_code_only_tests import (  # noqa: E402
    REL_TEST_FILE,
    ExitCodeOnlyFixture,
)


class ExitCodeOnlyMarkersTest(ExitCodeOnlyFixture):
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

    def test_deletion_after_nonadjacent_marker_does_not_select_weak_test(self) -> None:
        weak = "def test_x():\n    rc = run_checker()\n    assert rc == 0\n"
        prefix = "# exit-code-only: silent status probe\n# ordinary\n"
        target = self._commit_file(REL_TEST_FILE, prefix + "# extra\n\n" + weak)
        target.write_text(prefix + "\n" + weak, encoding="utf-8")
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


if __name__ == "__main__":
    unittest.main()
