#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exit-code-only checker tests for classification."""

from __future__ import annotations

import os
import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_exit_code_only_tests as checker  # noqa: E402
from scripts.ci.tests.test_check_exit_code_only_tests import (  # noqa: E402
    REL_TEST_FILE,
    ExitCodeOnlyFixture,
)
from scripts.lib.safe_subprocess import CommandOutputLimitExceeded, CommandTimedOut  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402


class ExitCodeOnlyClassificationTest(ExitCodeOnlyFixture):
    def test_r11_f1_nested_rebinding(self) -> None:
        source = (
            "def test_output():\n    proc = run_checker()\n    value = proc.returncode\n"
            "    (value,), label = ((len(proc.stdout),), 'out')\n    assert value == 0\n"
            "def test_rc():\n    rc, out = run_checker()\n    assert rc == 0\n"
        )
        self._commit_file(REL_TEST_FILE, source)
        rc, out, err = self._run_checker()
        self.assertEqual(rc, 0, err)
        self.assertIn("test_rc: asserts only exit code 0", out)
        self.assertNotIn("test_output:", out)

    def test_r11_f2_keyword_boolean(self) -> None:
        source = (
            "def test_false(self):\n    proc = run_checker()\n    self.assertFalse(expr=proc.returncode)\n"
            "def test_true(self):\n    proc = run_checker()\n    self.assertTrue(expr=proc.returncode == 0)\n"
            "def test_output(self):\n    proc = run_checker()\n    self.assertTrue(expr=proc.stdout == '')\n"
        )
        self._commit_file(REL_TEST_FILE, source)
        rc, out, err = self._run_checker()
        self.assertEqual(rc, 0, err)
        self.assertIn("test_false: asserts only exit code 0", out)
        self.assertIn("test_true: asserts only exit code 0", out)
        self.assertNotIn("test_output:", out)

    def test_r11_f3_literal_loop_target(self) -> None:
        source = (
            "def test_weak():\n    proc = run_checker()\n"
            "    for value in [(value := proc.returncode)]:\n        assert value == 0\n"
            "def test_output():\n    proc = run_checker()\n"
            "    for value in [len(proc.stdout)]:\n        assert value == 0\n"
        )
        self._commit_file(REL_TEST_FILE, source)
        rc, out, err = self._run_checker()
        self.assertEqual(rc, 0, err)
        self.assertIn("test_weak: asserts only exit code 0", out)
        self.assertNotIn("test_output:", out)

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
                CommandTimedOut(("git",), 1, None, None),
                CommandOutputLimitExceeded(("git",), 1, None, None),
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
        original: Callable[..., object] = run_command

        def ambient_order_file(*args: object, **kwargs: object) -> object:
            env = kwargs["env"]
            assert isinstance(env, dict)
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


if __name__ == "__main__":
    unittest.main()
