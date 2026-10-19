#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for scripts/ci/ci_expressions.py (ADR-2169)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import ci_expressions as gx

CONTEXT = {
    "github": {
        "event_name": "pull_request",
        "event": {
            "pull_request": {
                "draft": False,
                "labels": [{"name": "a"}, {"name": "B"}],
                "head": {"repo": {"full_name": "x/y"}},
            }
        },
        "repository": "x/y",
    },
    "needs": {"tier": {"outputs": {"light": "true", "full": ""}}},
}


def ev(text: str) -> object:
    return gx.evaluate(text, CONTEXT)


class Evaluate(unittest.TestCase):
    def test_comparison_and_logic(self) -> None:
        self.assertTrue(ev("github.event_name == 'pull_request'"))
        self.assertFalse(ev("github.event_name != 'pull_request'"))
        self.assertTrue(
            ev("github.event_name == 'push' || github.event.pull_request.draft == false")
        )
        self.assertFalse(
            ev("needs.tier.outputs.light == 'true' && needs.tier.outputs.full == 'true'")
        )

    def test_precedence_and_grouping(self) -> None:
        self.assertTrue(ev("true || false && false"))
        self.assertFalse(ev("(true || false) && false"))
        self.assertTrue(ev("!false && !(false || false)"))

    def test_strings_compare_without_case(self) -> None:
        self.assertTrue(ev("github.repository == 'X/Y'"))

    def test_null_equals_false_as_the_documentation_says(self) -> None:
        self.assertTrue(ev("github.event.push.missing == false"))
        self.assertFalse(ev("github.event.push.missing == 'true'"))

    def test_and_or_return_an_operand(self) -> None:
        self.assertEqual(ev("'a' && 'b'"), "b")
        self.assertEqual(ev("'' || 'c'"), "c")

    def test_functions(self) -> None:
        self.assertTrue(ev("contains(github.event.pull_request.labels.*.name, 'b')"))
        self.assertFalse(ev("contains(github.event.pull_request.labels.*.name, 'c')"))
        self.assertTrue(ev("startsWith(github.repository, 'x/')"))
        self.assertTrue(ev("contains(fromJson('[\"p\",\"q\"]'), 'Q')"))
        self.assertTrue(ev("contains(fromJson(needs.tier.outputs.full || '[\"z\"]'), 'z')"))

    def test_status_functions_and_wrapper(self) -> None:
        self.assertTrue(ev("${{ always() }}"))
        self.assertTrue(ev("${{ !cancelled() && needs.tier.outputs.light == 'true' }}"))
        self.assertTrue(gx.uses_status_function("always() && x"))
        self.assertTrue(gx.uses_status_function("!cancelled() && x"))
        self.assertFalse(gx.uses_status_function("needs.tier.outputs.light == 'true'"))

    def test_quoted_quote_and_numbers(self) -> None:
        self.assertTrue(ev("'it''s' == 'IT''S'"))
        self.assertTrue(ev("1 < 2 && 2 <= 2 && 3 > 2"))

    def test_a_missing_path_is_null(self) -> None:
        self.assertIsNone(ev("needs.nothing.outputs.x"))


class Refusals(unittest.TestCase):
    def test_unknown_function_is_refused(self) -> None:
        with self.assertRaises(gx.ExpressionError):
            ev("hashFiles('x') == 'y'")

    def test_unbalanced_parentheses_are_refused(self) -> None:
        for text in ("(true", "true)", "contains(a, b"):
            with self.subTest(text=text), self.assertRaises(gx.ExpressionError):
                ev(text)

    def test_garbage_is_refused(self) -> None:
        with self.assertRaises(gx.ExpressionError):
            ev("a @ b")

    def test_a_dangling_operator_is_refused(self) -> None:
        with self.assertRaises(gx.ExpressionError):
            ev("true &&")


if __name__ == "__main__":
    unittest.main()
