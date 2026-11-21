#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exit-code-only checker tests for flow."""

from __future__ import annotations

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


class ExitCodeOnlyFlowTest(ExitCodeOnlyFixture):
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

    def test_nonassignment_rebindings_invalidate_returncode(self) -> None:
        prefix = (
            "async def test_case():\n    proc = run_checker()\n"
            "    value = proc.returncode\n    assert value == 0\n"
        )
        cases = {
            "for": "    for value in [len(proc.stdout)]:\n        assert value == 0\n",
            "async_for": "    async for value in items:\n        assert value == 0\n",
            "with": "    with context() as value:\n        assert value == 0\n",
            "async_with": "    async with context() as value:\n        assert value == 0\n",
            "except": "    try:\n        pass\n    except Exception as value:\n        assert value == 0\n",
            "capture": "    match x:\n        case value:\n            assert value == 0\n",
            "star": "    match x:\n        case [*value]:\n            assert value == 0\n",
            "mapping": "    match x:\n        case {**value}:\n            assert value == 0\n",
            "del": "    del value\n    assert value == 0\n",
            "import": "    import example as value\n    assert value == 0\n",
            "from_import": "    from example import value\n    assert value == 0\n",
        }
        for name, body in cases.items():
            with self.subTest(form=name):
                self.assertEqual(checker.file_findings(prefix + body, REL_TEST_FILE), [])

    def test_loop_body_can_establish_returncode_after_rebinding(self) -> None:
        source = (
            "def test_case():\n    value = run_checker()\n"
            "    for value in [1]:\n        rc = run_checker()\n        assert rc == 0\n"
        )
        self.assertEqual(len(checker.file_findings(source, REL_TEST_FILE)), 1)

    def test_match_guard_binding_is_seen_in_case_body(self) -> None:
        source = (
            "def test_case():\n    match True:\n"
            "        case _ if (rc := main([])) == 0:\n            assert rc == 0\n"
        )
        findings = checker.file_findings(source, REL_TEST_FILE)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].message, "asserts only exit code 0")

    def test_match_guard_bindings_reach_later_cases(self) -> None:
        source = (
            "def test_case():\n    proc = run_checker()\n"
            "    match True:\n"
            "        case False if (value := proc.returncode) == 0:\n            pass\n"
            "        case _:\n            assert value == 0\n"
        )
        self.assertEqual(len(checker.file_findings(source, REL_TEST_FILE)), 1)

    def test_literal_loop_target_retains_returncode(self) -> None:
        source = (
            "def test_case():\n    proc = run_checker()\n"
            "    for value in [(value := proc.returncode)]:\n        assert value == 0\n"
        )
        self.assertEqual(len(checker.file_findings(source, REL_TEST_FILE)), 1)


if __name__ == "__main__":
    unittest.main()
