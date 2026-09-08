# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise real cppcheck modeling/depth; missing tools are errors."""

from __future__ import annotations

import json
import os
import re
import runpy
import shutil
import subprocess
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import cast

ROOT = Path(__file__).resolve().parents[3]
COMMAND = cast(
    Callable[[str, Path, Path], list[str]],
    runpy.run_path(str(ROOT / "scripts/ci/lint-configured.py"))["cppcheck_arguments"],
)

HEADERS_CONTROL = """#include "feature/feature_collector.h"
int main(void) {
    const VmafModel model = {0};
    const VmafFeatureCollector collector = {0};
    FeatureVector vector = {0};
    double score = 0.0;
    return (int)model.n_features + (int)collector.cnt +
        vmaf_feature_vector_get_score(&vector, &score, 0);
}
"""


class CppcheckPosixModelTests(unittest.TestCase):
    def setUp(self) -> None:
        binary = shutil.which(os.environ.get("CPPCHECK_BIN", "cppcheck"))
        if binary is None:
            raise RuntimeError("cppcheck is required for the native model contract")
        self.binary = binary
        self.temp = tempfile.TemporaryDirectory(prefix="vmafx-cppcheck-posix-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def analyze(
        self, text: str, *, language: str = "c++", extra: tuple[str, ...] = ()
    ) -> tuple[int, list[tuple[str, str]], str]:
        """Use production argv and a real one-TU database, without Git or a build."""
        source = self.directory / ("control.c" if language == "c" else "control.cpp")
        source.write_text(text, encoding="utf-8")
        database = self.directory / "compile_commands.json"
        database.write_text(
            json.dumps(
                [
                    {
                        "directory": str(self.directory),
                        "file": str(source),
                        "arguments": [
                            "cc" if language == "c" else "c++",
                            "-std=c99" if language == "c" else "-std=c++17",
                            f"-I{ROOT / 'core/src'}",
                            f"-I{ROOT / 'core/include'}",
                            "-c",
                            str(source),
                        ],
                    }
                ]
            ),
            encoding="utf-8",
        )
        before = database.read_bytes()
        result = subprocess.run(  # noqa: S603 -- resolved tool, fixture argv, no shell
            [*COMMAND(self.binary, ROOT, database), "--template={severity}:{id}", *extra],
            cwd=self.directory,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(database.read_bytes(), before)
        diagnostics = re.findall(
            r"^(error|warning|style|performance|portability|information):([A-Za-z0-9_]+)$",
            result.stderr,
            re.MULTILINE,
        )
        return result.returncode, diagnostics, result.stdout + result.stderr

    def test_real_shared_headers_are_plain_c_aggregates_in_c_and_cpp(self) -> None:
        for language in ("c", "c++"):
            with self.subTest(language=language):
                code, diagnostics, output = self.analyze(HEADERS_CONTROL, language=language)
                self.assertEqual(code, 0, output)
                self.assertFalse(
                    any(severity != "information" for severity, _identifier in diagnostics), output
                )

    def test_exhaustive_analysis_finishes_real_branch_budget_control(self) -> None:
        # Eight early returns exceed 2.21's four forward branches. Older supported
        # tools (notably Ubuntu 24.04's 2.13) have no such cutoff/diagnostic.
        source = (
            "int main(int argc, char **argv) {\n(void)argv;\n"
            + "".join(f"if (argc == {value}) {{ return {value}; }}\n" for value in range(8))
            + "return argc;\n}\n"
        )
        code, diagnostics, output = self.analyze(
            source, language="c", extra=("--check-level=normal",)
        )
        version = subprocess.run(  # noqa: S603 -- resolved analyzer, fixed argv
            [self.binary, "--version"], capture_output=True, text=True, check=True
        ).stdout
        match = re.search(r"Cppcheck (\d+)\.(\d+)", version)
        self.assertIsNotNone(match, version)
        assert match is not None
        if tuple(map(int, match.groups())) == (2, 21):
            self.assertEqual(code, 1, output)
            self.assertIn(("information", "normalCheckLevelMaxBranches"), diagnostics, output)
        self.assertEqual(
            code, int(("information", "normalCheckLevelMaxBranches") in diagnostics), output
        )
        self.assertTrue(
            all(
                severity == "information"
                and identifier in {"normalCheckLevelMaxBranches", "checkersReport"}
                for severity, identifier in diagnostics
            ),
            output,
        )
        code, diagnostics, output = self.analyze(source, language="c")
        self.assertEqual(code, 0, output)
        self.assertTrue(
            all(item == ("information", "checkersReport") for item in diagnostics), output
        )

    def test_actual_uninitialized_c_member_read_still_fails(self) -> None:
        source = """#include "model.h"
int main(void) {
    VmafModel model;
    return (int)model.n_features;
}
"""
        for language in ("c", "c++"):
            with self.subTest(language=language):
                code, diagnostics, output = self.analyze(source, language=language)
                self.assertNotEqual(code, 0, output)
                self.assertIn(
                    "uninitvar", {identifier for _severity, identifier in diagnostics}, output
                )

    def test_actual_cpp_constructor_defect_still_fails(self) -> None:
        code, diagnostics, output = self.analyze(
            "struct Broken { int value; Broken() {} };\n"
            "int main() { const Broken broken; return broken.value; }\n"
        )
        self.assertNotEqual(code, 0, output)
        self.assertIn(
            "uninitMemberVar", {identifier for _severity, identifier in diagnostics}, output
        )

    def test_real_cpp_class_member_initializer_warning_remains_enabled(self) -> None:
        code, diagnostics, output = self.analyze(
            "#include <string>\nstruct Broken { std::string text; int value; };\n"
            "int main() { Broken broken; return broken.value; }\n"
        )
        self.assertNotEqual(code, 0, output)
        ids = {identifier for _severity, identifier in diagnostics}
        self.assertIn("uninitvar", ids, output)
        # Older distro tools do not implement this newer diagnostic; when the
        # installed analyzer supports it, the model must leave it enabled.
        listed = subprocess.run(  # noqa: S603 -- resolved tool, fixed argv
            [self.binary, "--errorlist"], capture_output=True, text=True, check=True
        )
        supported = set(re.findall(r'<error id="([A-Za-z0-9_]+)"', listed.stdout + listed.stderr))
        if "uninitMemberVarNoCtor" in supported:
            self.assertIn("uninitMemberVarNoCtor", ids, output)

    def test_missing_library_is_a_failure(self) -> None:
        missing = self.directory / "not-installed.cfg"
        code, _diagnostics, output = self.analyze(HEADERS_CONTROL, extra=(f"--library={missing}",))
        self.assertNotEqual(code, 0, output)
        self.assertIn("Failed to load library configuration file", output)


if __name__ == "__main__":
    unittest.main()
