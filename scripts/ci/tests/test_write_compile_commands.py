#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression tests for the explicit Ninja compilation-database exporter."""

from __future__ import annotations

import contextlib
import io
import json
import runpy
import sys
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ci/write-compile-commands.py"
EXPORT_MAIN = runpy.run_path(str(SCRIPT))["main"]


@dataclass(frozen=True)
class ExportResult:
    returncode: int
    stdout: str
    stderr: str


class CompileCommandsExportTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        (self.build / "build.ninja").write_text("# fixture\n", encoding="utf-8")
        self.ninja = self.root / "ninja"
        self.log = self.root / "argv.jsonl"
        self.entries: object = [
            {
                "directory": str(self.build),
                "command": "cc -c ../source.c -o source.c.o",
                "file": "../source.c",
                "output": "source.c.o",
            },
            {
                "directory": str(self.build),
                "arguments": ["c++", "-c", "../source.cpp", "-o", "source.cpp.o"],
                "file": "../source.cpp",
                "output": "source.cpp.o",
            },
        ]
        self.rules = ["CUSTOM_COMMAND", "c_COMPILER", "cpp_COMPILER", "cpp_LINKER"]
        self.returncode = 0
        self.raw_output: str | None = None
        self.write_ninja()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_ninja(self) -> None:
        payload = self.raw_output if self.raw_output is not None else json.dumps(self.entries)
        source = (
            f"#!{sys.executable}\n"
            "import json, sys\n"
            "from pathlib import Path\n"
            f"log = Path({str(self.log)!r})\n"
            "with log.open('a', encoding='utf-8') as handle:\n"
            "    handle.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "if sys.argv[-2:] == ['-t', 'rules']:\n"
            f"    print({chr(10).join(self.rules)!r})\n"
            "elif '-t' in sys.argv and 'compdb' in sys.argv:\n"
            f"    print({payload!r})\n"
            f"    raise SystemExit({self.returncode})\n"
            "else:\n"
            "    raise SystemExit('unexpected argv')\n"
        )
        self.ninja.write_text(source, encoding="utf-8")
        self.ninja.chmod(0o755)

    def run_export(self) -> ExportResult:
        stdout = io.StringIO()
        stderr = io.StringIO()
        main = EXPORT_MAIN
        if not isinstance(main, Callable):
            self.fail("exporter main is not callable")
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            returncode = main(["--build-dir", str(self.build), "--ninja", str(self.ninja)])
        return ExportResult(returncode, stdout.getvalue(), stderr.getvalue())

    def calls(self) -> list[list[str]]:
        return [json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines()]

    def assert_preserved_on_failure(self) -> ExportResult:
        database = self.build / "compile_commands.json"
        original = b'[{"existing": "valid"}]\n'
        database.write_bytes(original)
        result = self.run_export()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(database.read_bytes(), original)
        self.assertEqual(list(self.build.glob(".compile_commands.*.tmp")), [])
        return result

    def test_exports_only_explicit_compiler_rules(self) -> None:
        result = self.run_export()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            json.loads((self.build / "compile_commands.json").read_text()), self.entries
        )
        calls = self.calls()
        self.assertEqual(calls[0][-2:], ["-t", "rules"])
        self.assertEqual(
            calls[1][-4:],
            ["-t", "compdb", "c_COMPILER", "cpp_COMPILER"],
        )
        self.assertNotIn("CUSTOM_COMMAND", calls[1])
        self.assertNotIn("cpp_LINKER", calls[1])

    def test_missing_manifest_preserves_existing_database(self) -> None:
        (self.build / "build.ninja").unlink()
        result = self.assert_preserved_on_failure()
        self.assertIn("Ninja manifest not found", result.stderr)

    def test_missing_compiler_rule_preserves_existing_database(self) -> None:
        self.rules.remove("cpp_COMPILER")
        self.write_ninja()
        result = self.assert_preserved_on_failure()
        self.assertIn("missing compiler rules: cpp_COMPILER", result.stderr)

    def test_nonzero_ninja_preserves_existing_database(self) -> None:
        self.returncode = 7
        self.write_ninja()
        result = self.assert_preserved_on_failure()
        self.assertIn("ninja -t compdb", result.stderr)

    def test_empty_database_preserves_existing_database(self) -> None:
        self.entries = []
        self.write_ninja()
        result = self.assert_preserved_on_failure()
        self.assertIn("database is empty", result.stderr)

    def test_malformed_database_preserves_existing_database(self) -> None:
        self.entries = {"not": "an array"}
        self.write_ninja()
        result = self.assert_preserved_on_failure()
        self.assertIn("must be an array", result.stderr)

    def test_truncated_json_preserves_existing_database(self) -> None:
        self.raw_output = '[{"directory":'
        self.write_ninja()
        result = self.assert_preserved_on_failure()
        self.assertIn("Ninja emitted invalid JSON", result.stderr)

    def test_invalid_entry_preserves_existing_database(self) -> None:
        self.entries = [{"directory": str(self.build), "file": "linked-output"}]
        self.write_ninja()
        result = self.assert_preserved_on_failure()
        self.assertIn("not a C/C++ source", result.stderr)

    def test_make_and_ci_consumers_call_the_exporter(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        workflow = (ROOT / ".github/workflows/lint-and-format.yml").read_text(encoding="utf-8")
        nightly = (ROOT / ".github/workflows/nightly.yml").read_text(encoding="utf-8")
        invocation = "scripts/ci/write-compile-commands.py"
        self.assertEqual(makefile.count(invocation), 3)
        self.assertEqual(workflow.count(invocation), 4)
        self.assertEqual(nightly.count(invocation), 1)
        lint_target = makefile.split("lint-c:", 1)[1].split("# ADR-1142", 1)[0]
        self.assertLess(
            lint_target.index(invocation), lint_target.index("scripts/ci/lint-configured.py")
        )

    def test_precommit_runs_exporter_contract_for_every_owner(self) -> None:
        config = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        hook = config.split("- id: test-compile-commands-export", 1)[1].split("- id:", 1)[0]
        for path in (
            "Makefile",
            "scripts/ci/write-compile-commands.py",
            "scripts/ci/tests/test_write_compile_commands.py",
            ".github/workflows/lint-and-format.yml",
            ".github/workflows/nightly.yml",
            ".pre-commit-config.yaml",
        ):
            with self.subTest(path=path):
                self.assertRegex(path, config_pattern(hook))
        self.assertIn("-p test_write_compile_commands.py", hook)


def config_pattern(hook: str) -> str:
    prefix = "files: '"
    start = hook.index(prefix) + len(prefix)
    return hook[start : hook.index("'", start)]


if __name__ == "__main__":
    unittest.main()
