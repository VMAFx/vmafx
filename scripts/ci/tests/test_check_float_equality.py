#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the compile-only float-equality check (issue 3303)."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
FIXTURES = Path(__file__).with_name("fixtures") / "float_equality"
SPEC = importlib.util.spec_from_file_location(
    "check_float_equality", ROOT / "scripts/ci/check_float_equality.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FloatEqualityTests(unittest.TestCase):
    def test_planted_header_declared_shape_is_not_exempt(self) -> None:
        line = (FIXTURES / "planted.c").read_text(encoding="utf-8").splitlines()[6]
        self.assertFalse(MODULE.diagnostic_is_exempt(line))

    def test_literal_operand_is_exempt(self) -> None:
        self.assertTrue(MODULE.diagnostic_is_exempt("if (score == 0.0f) return 1;"))
        self.assertTrue(MODULE.diagnostic_is_exempt("if (0.0f == score) return 1;"))

    def test_nan_idiom_is_exempt(self) -> None:
        self.assertTrue(MODULE.diagnostic_is_exempt("if (value != value) return 1;"))

    def test_planted_diagnostic_is_reported(self) -> None:
        output = (
            f"{FIXTURES / 'planted.c'}:6:12: error: comparing floating point with == or != "
            "is unsafe [-Wfloat-equal]\n"
        )
        self.assertEqual(
            MODULE.findings(output, FIXTURES / "planted.c"),
            [
                f"{FIXTURES / 'planted.c'}:6: comparing floating point with == or != is unsafe"
                "[-Wfloat-equal]"
            ],
        )

    def test_check_rejects_the_planted_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build),
                            "file": str(FIXTURES / "planted.c"),
                            "arguments": ["clang", "-c", str(FIXTURES / "planted.c")],
                        }
                    ]
                ),
                encoding="utf-8",
            )
            diagnostic = (
                f"{FIXTURES / 'planted.c'}:6:12: error: comparing floating point with == or != "
                "is unsafe [-Wfloat-equal]"
            )
            with mock.patch.object(
                MODULE,
                "run",
                return_value=SimpleNamespace(returncode=1, stdout="", stderr=diagnostic),
            ):
                problems = MODULE.check(
                    build, ROOT, prefix="scripts/ci/tests/fixtures/float_equality/"
                )
        self.assertEqual(len(problems), 1)
        self.assertIn("planted.c:6", problems[0])

    def test_check_is_wired_into_the_c_cpp_lint_job(self) -> None:
        workflow = (ROOT / ".github/workflows/lint-and-format.yml").read_text(encoding="utf-8")
        self.assertIn(
            "python3 scripts/ci/check_float_equality.py --build-dir build",
            workflow,
        )


if __name__ == "__main__":
    unittest.main()
