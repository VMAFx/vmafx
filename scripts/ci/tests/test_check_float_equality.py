#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the compile-only float-equality check (issue 3303)."""

from __future__ import annotations

import json
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
FIXTURES = Path(__file__).with_name("fixtures") / "float_equality"
sys.path.insert(0, str(ROOT))

from scripts.ci import check_float_equality as MODULE  # noqa: E402


class FloatEqualityTests(unittest.TestCase):
    def test_planted_header_declared_shape_is_not_exempt(self) -> None:
        line = (FIXTURES / "planted.c").read_text(encoding="utf-8").splitlines()[6]
        self.assertFalse(MODULE.diagnostic_is_exempt(line, line.index("==") + 1))

    def test_literal_operand_is_exempt(self) -> None:
        for line in (
            "if (score == 0.0f) return 1;",
            "if (0.0f == score) return 1;",
            "return score == -.5f;",
            "return +0x1.8p-2L == score;",
            "return score == 12ULL;",
        ):
            with self.subTest(line=line):
                self.assertTrue(MODULE.diagnostic_is_exempt(line, line.index("==") + 1))

    def test_nan_idiom_is_exempt(self) -> None:
        line = "if (value1 != value1) return 1;"
        self.assertTrue(MODULE.diagnostic_is_exempt(line, line.index("!=") + 1))

    def test_ambiguous_source_does_not_waive_a_diagnostic(self) -> None:
        cases = (
            ("return a == 1.25f;", 1),
            ("return a == 1.25f;", 0),
            ("return a == 1.25f;", 99),
            ("    2.0f == b;", 10),  # The previous line may end with '*'.
            ("return a == 2.0f", 10),  # The next line may start with '*'.
            ("return a <= 2 == b;", 15),
            ("return a != b.a;", 10),
            ('return a == b && consume("1.25f == c");', 10),
        )
        for line, column in cases:
            with self.subTest(line=line, column=column):
                self.assertFalse(MODULE.diagnostic_is_exempt(line, column))

    def test_planted_diagnostic_is_reported(self) -> None:
        output = (
            f"{FIXTURES / 'planted.c'}:6:12: error: comparing floating point with == or != "
            "is unsafe [-Wfloat-equal]\n"
        )
        self.assertEqual(
            MODULE.findings(output, ROOT),
            [
                f"{FIXTURES / 'planted.c'}:6:12: comparing floating point with == or != is unsafe"
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

    def test_gcc_werror_diagnostic_is_reported(self) -> None:
        output = (
            f"{FIXTURES / 'planted.c'}:7:12: error: comparing floating point with == or != "
            "is unsafe [-Werror=float-equal]\n"
        )
        self.assertEqual(
            MODULE.findings(output, ROOT),
            [
                f"{FIXTURES / 'planted.c'}:7:12: comparing floating point with == or != is unsafe"
                "[-Wfloat-equal]"
            ],
        )

    def test_clang_werror_diagnostic_is_reported(self) -> None:
        output = (
            f"{FIXTURES / 'planted.c'}:7:12: error: comparing floating point with == or != "
            "is unsafe [-Werror,-Wfloat-equal]\n"
        )
        self.assertEqual(
            MODULE.findings(output, ROOT),
            [
                f"{FIXTURES / 'planted.c'}:7:12: comparing floating point with == or != is unsafe"
                "[-Wfloat-equal]"
            ],
        )

    def test_real_compiler_reports_a_float_comparison(self) -> None:
        compiler = shutil.which("gcc") or shutil.which("gcc-15") or shutil.which("clang")
        if compiler is None:
            self.skipTest("gcc/clang is not on PATH")
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            source = build / "comparison.c"
            source.write_text(
                "int compare(float a, float b)\n{\n    return a == b;\n}\n",
                encoding="utf-8",
            )
            (build / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build),
                            "file": str(source),
                            "arguments": [compiler, "-c", str(source)],
                        }
                    ]
                ),
                encoding="utf-8",
            )
            problems = MODULE.check(build, build, compiler=compiler, prefix="")
        self.assertTrue(any("comparison.c:3:" in problem for problem in problems), problems)

    def check_output(self, output: str, line: str, returncode: int = 1) -> list[str]:
        source = Path("comparison.c")
        with (
            mock.patch.object(MODULE, "load_units", return_value=[(source, ["gcc"], ROOT)]),
            mock.patch.object(MODULE, "source_line", return_value=line),
            mock.patch.object(
                MODULE,
                "run",
                return_value=SimpleNamespace(returncode=returncode, stdout="", stderr=output),
            ),
        ):
            return MODULE.check(ROOT, ROOT)

    def test_check_accepts_only_exempt_float_errors(self) -> None:
        for tag in MODULE.FLOAT_EQUAL_TAGS:
            for line in ("return a == 1.25f;", "return a != a;"):
                with self.subTest(tag=tag, line=line):
                    output = f"comparison.c:1:10: error: unsafe comparison {tag}\n"
                    self.assertEqual(self.check_output(output, line), [])

    def test_check_keeps_non_float_errors_with_float_diagnostics(self) -> None:
        float_error = "comparison.c:1:10: error: unsafe [-Werror=float-equal]\n"
        other_errors = (
            "comparison.c:2:3: error: unknown identifier 'missing'",
            "comparison.c:2: fatal error: missing.h: No such file or directory",
            "gcc: error: unrecognized command-line option '-invalid'",
        )
        for line in ("return a == 1.25f;", "return a == b;"):
            for error in other_errors:
                with self.subTest(line=line, error=error):
                    problems = self.check_output(float_error + error, line)
                    self.assertTrue(any(error in problem for problem in problems), problems)

    def test_unexplained_failure_is_not_excused_by_a_float_warning(self) -> None:
        output = "comparison.c:1:10: warning: unsafe [-Wfloat-equal]\n"
        self.assertTrue(self.check_output(output, "return a == 1.25f;"))
        self.assertTrue(self.check_output("", ""))

    def test_abnormal_exit_is_not_excused_by_an_exempt_float_error(self) -> None:
        output = "comparison.c:1:10: error: unsafe [-Werror=float-equal]\n"
        for returncode in (2, -9):
            with self.subTest(returncode=returncode):
                self.assertTrue(self.check_output(output, "return a == 1.25f;", returncode))

    def real_compiler_problems(self, body: str | None, compiler: str | None = None) -> list[str]:
        compiler = (
            compiler or shutil.which("gcc") or shutil.which("gcc-15") or shutil.which("clang")
        )
        if compiler is None:
            self.skipTest("gcc/clang is not on PATH")
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            source = FIXTURES / "planted.c" if body is None else build / "comparison.c"
            if body is not None:
                source.write_text(body, encoding="utf-8")
            (build / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build),
                            "file": str(source),
                            "arguments": [compiler, "-c", str(source)],
                        }
                    ]
                ),
                encoding="utf-8",
            )
            return MODULE.check(build, ROOT if body is None else build, prefix="")

    def test_real_compiler_rejects_planted_header_declared_comparison(self) -> None:
        problems = self.real_compiler_problems(None)
        self.assertTrue(any("planted.c:7:" in problem for problem in problems), problems)

    def real_compilers(self) -> list[str]:
        compilers = [shutil.which("gcc") or shutil.which("gcc-15"), shutil.which("clang")]
        available = [compiler for compiler in compilers if compiler is not None]
        if not available:
            self.skipTest("gcc/clang is not on PATH")
        return available

    def test_real_compilers_reject_each_unsafe_comparison(self) -> None:
        cases = (
            ("a == 1.25f || b == c", "b == c"),
            ("a != a || b == c", "b == c"),
            ("a * 2.0f == b", "2.0f == b"),
            ("a == 2 * b", "a == 2"),
            ("v1 == v2", "v1 == v2"),
        )
        functions = [
            f"int compare_{i}(float a, float b, float c, float v1, float v2) {{ return {expr}; }}"
            for i, (expr, _) in enumerate(cases)
        ]
        expected = [
            f"comparison.c:{i}:{line.index('==', line.index(unsafe)) + 1}:"
            for i, (line, (_, unsafe)) in enumerate(zip(functions, cases, strict=True), start=1)
        ]
        for compiler in self.real_compilers():
            with self.subTest(compiler=compiler):
                problems = self.real_compiler_problems("\n".join(functions) + "\n", compiler)
                self.assertEqual(len(problems), len(expected), problems)
                for location in expected:
                    self.assertTrue(any(location in problem for problem in problems), problems)
                self.assertTrue(
                    all(MODULE.FLOAT_EQUAL in problem for problem in problems), problems
                )

    def test_real_compilers_accept_only_exempt_comparisons(self) -> None:
        source = (
            "int literal(float a) { return a == 1.25f || 1.25f == a; }\n"
            "int nan_test(float a) { return a != a; }\n"
        )
        for compiler in self.real_compilers():
            with self.subTest(compiler=compiler):
                self.assertEqual(self.real_compiler_problems(source, compiler), [])

    def test_real_compiler_rejects_non_float_error(self) -> None:
        problems = self.real_compiler_problems("int compare(void) { return missing; }\n")
        self.assertTrue(any("missing" in problem for problem in problems), problems)

    def test_real_compiler_rejects_non_float_error_alongside_exempt_comparison(self) -> None:
        problems = self.real_compiler_problems(
            "int compare(float a) { return a == 1.25f; }\n" "int broken(void) { return missing; }\n"
        )
        self.assertTrue(any("missing" in problem for problem in problems), problems)

    def test_check_is_wired_into_the_c_cpp_lint_job(self) -> None:
        workflow = (ROOT / ".github/workflows/lint-and-format.yml").read_text(encoding="utf-8")
        job = re.search(r"(?ms)^  clang-tidy:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n)", workflow)
        if job is None:
            self.fail("clang-tidy job is missing from lint-and-format.yml")
        body = job.group(1)
        compile_step = body.index("- name: Generate compile_commands.json")
        check_step = body.index("- name: Check float equality in core/test")
        tidy_step = body.index("- name: Run clang-tidy on changed files")
        self.assertLess(compile_step, check_step)
        self.assertLess(check_step, tidy_step)
        self.assertIn("python3 scripts/ci/check_float_equality.py --build-dir build", body)


if __name__ == "__main__":
    unittest.main()
