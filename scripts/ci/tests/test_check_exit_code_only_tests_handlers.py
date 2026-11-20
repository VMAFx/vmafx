#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exit-code-only checker tests for handlers."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_exit_code_only_tests as checker  # noqa: E402
from scripts.ci.tests.test_check_exit_code_only_tests import (  # noqa: E402
    REL_TEST_FILE,
    ExitCodeOnlyFixture,
)


class ExitCodeOnlyHandlersTest(ExitCodeOnlyFixture):
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

    def _check_handler_source(self, body: str, expected: int) -> None:
        target = self._commit_file(REL_TEST_FILE, "# base\n")
        target.write_text("def test_x():\n" + body, encoding="utf-8")
        rc, out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, expected, err)
        if expected:
            self.assertIn("asserts only exit code 0", err)
        else:
            self.assertIn("0 exit-code-only test findings", out)
            self.assertNotIn("test_x", err)

    def test_handler_sees_intermediate_binding(self) -> None:
        self._check_handler_source(
            "    try:\n        rc = run_checker()\n        might_raise()\n        rc = 3\n"
            "    except ValueError:\n        assert rc == 0\n",
            1,
        )

    def test_handler_sees_nested_intermediate_binding(self) -> None:
        self._check_handler_source(
            "    try:\n        with ctx():\n            rc = run_checker()\n"
            "            might_raise()\n            rc = 3\n"
            "    except ValueError:\n        assert rc == 0\n",
            1,
        )

    def test_exception_group_handler_sees_intermediate_binding(self) -> None:
        self._check_handler_source(
            "    try:\n        rc = run_checker()\n        might_raise()\n        rc = 3\n"
            "    except* ValueError:\n        assert rc == 0\n",
            1,
        )

    def test_handler_sees_bindings_in_nested_control_flow(self) -> None:
        prefixes = (
            "if flag:",
            "for item in items:",
            "while flag:",
            "match flag:\n            case True:",
            "try:",
        )
        for prefix in prefixes:
            with self.subTest(prefix=prefix):
                indent = "                " if prefix.startswith("match") else "            "
                suffix = (
                    "        except ValueError:\n            pass\n" if prefix == "try:" else ""
                )
                source = (
                    "def test_x():\n    try:\n        "
                    + prefix
                    + "\n"
                    + indent
                    + "rc = run_checker()\n"
                    + indent
                    + "might_raise()\n"
                    + indent
                    + "rc = 3\n"
                    + suffix
                    + "    except ValueError:\n        assert rc == 0\n"
                )
                self.assertEqual(len(checker.file_findings(source, REL_TEST_FILE)), 1)

    def test_handler_excludes_binding_replaced_before_try(self) -> None:
        self._check_handler_source(
            "    rc = run_checker()\n    rc = 3\n    try:\n        might_raise()\n"
            "    except ValueError:\n        assert rc == 0\n",
            0,
        )

    def test_handler_other_assertion_is_strong(self) -> None:
        self._check_handler_source(
            "    try:\n        out = run_checker()\n        rc = run_checker()\n"
            "        assert rc == 0\n    except ValueError:\n        assert out.stdout == 'x'\n",
            0,
        )

    def test_finally_sees_handler_entry_bindings(self) -> None:
        source = (
            "def test_x():\n    try:\n        rc = run_checker()\n        rc = 3\n"
            "    except ValueError:\n        rc = 3\n    finally:\n        assert rc == 0\n"
        )
        self.assertEqual(len(checker.file_findings(source, REL_TEST_FILE)), 1)

    def test_finally_sees_intermediate_body_binding(self) -> None:
        self._check_handler_source(
            "    try:\n        rc = run_checker()\n        might_raise()\n        rc = 3\n"
            "    finally:\n        assert rc == 0\n",
            1,
        )

    def test_finally_sees_intermediate_handler_binding(self) -> None:
        self._check_handler_source(
            "    try:\n        might_raise()\n    except ValueError:\n"
            "        rc = run_checker()\n        boom()\n        rc = 3\n"
            "    finally:\n        assert rc == 0\n",
            1,
        )

    def test_finally_sees_intermediate_else_binding(self) -> None:
        self._check_handler_source(
            "    try:\n        pass\n    except ValueError:\n        pass\n"
            "    else:\n        rc = run_checker()\n        boom()\n        rc = 3\n"
            "    finally:\n        assert rc == 0\n",
            1,
        )

    def test_finally_excludes_binding_replaced_before_try(self) -> None:
        self._check_handler_source(
            "    rc = run_checker()\n    rc = 3\n    try:\n        might_raise()\n"
            "    finally:\n        assert rc == 0\n",
            0,
        )

    def test_exception_group_finally_sees_nested_binding(self) -> None:
        self._check_handler_source(
            "    try:\n        pass\n    except* ValueError:\n        if flag:\n"
            "            rc = run_checker()\n            boom()\n            rc = 3\n"
            "    finally:\n        assert rc == 0\n",
            1,
        )

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


if __name__ == "__main__":
    unittest.main()
