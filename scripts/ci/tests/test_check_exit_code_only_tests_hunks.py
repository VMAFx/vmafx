#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exit-code-only checker tests for hunks."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci.tests.test_check_exit_code_only_tests import (  # noqa: E402
    REL_TEST_FILE,
    ExitCodeOnlyFixture,
)


class ExitCodeOnlyHunksTest(ExitCodeOnlyFixture):
    def _check_hunk_case(self, base: str, changed: str, expected: int) -> None:
        target = self._commit_file(REL_TEST_FILE, base)
        target.write_text(changed, encoding="utf-8")
        rc, _out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, expected, err)
        if expected == 1:
            self.assertIn("test_a:", err)

    def test_r11_f6_else_suite_boundary(self) -> None:
        source = (
            "if condition:\n    def test_a():\n        rc = run_checker()\n"
            "        assert rc == 0\nelse:\n    setting = 1\n"
        )
        target = self._commit_file(REL_TEST_FILE, source)
        target.write_text(source.replace("setting = 1", "setting = 2"), encoding="utf-8")
        rc, out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only test findings", out)
        self.assertNotIn("test_a:", err)

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


if __name__ == "__main__":
    unittest.main()
