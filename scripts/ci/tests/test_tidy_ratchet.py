# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Unit tests for scripts/ci/tidy-ratchet.py (ADR-1142)."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "tidy-ratchet.py"


def _load():
    spec = importlib.util.spec_from_file_location("tidy_ratchet", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


ratchet = _load()


class ParseDiagnostics(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "core" / "src").mkdir(parents=True)
        (self.root / "core" / "src" / "a.c").write_text("int x;\n", encoding="utf-8")
        (self.root / "core" / "src" / "a.h").write_text("", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_dedups_and_relativises(self) -> None:
        src = self.root / "core" / "src" / "a.c"
        out = "\n".join(
            [
                f"{src}:3:5: warning: use nullptr [modernize-use-nullptr]",
                f"{src}:3:5: warning: use nullptr [modernize-use-nullptr]",
                f"{src}:9:1: warning: too long [readability-function-size]",
                "/usr/include/stdio.h:1:1: warning: foo [bar-baz]",
                "12 warnings generated.",
            ]
        )
        diags, failed = ratchet.parse_diagnostics(out, self.root, self.root)
        self.assertFalse(failed)
        self.assertEqual(
            diags,
            {
                ("core/src/a.c", 3, 5, "modernize-use-nullptr"),
                ("core/src/a.c", 9, 1, "readability-function-size"),
            },
        )

    def test_warnings_as_errors_still_count(self) -> None:
        src = self.root / "core" / "src" / "a.c"
        out = f"{src}:1:1: error: discarded [cert-err33-c,-warnings-as-errors]"
        diags, failed = ratchet.parse_diagnostics(out, self.root, self.root)
        self.assertFalse(failed)
        self.assertEqual(len(diags), 1)

    def test_compile_error_fails_closed(self) -> None:
        src = self.root / "core" / "src" / "a.c"
        out = f"{src}:1:10: error: 'x.h' file not found [clang-diagnostic-error]"
        diags, failed = ratchet.parse_diagnostics(out, self.root, self.root)
        self.assertTrue(failed)
        self.assertEqual(diags, set())

    def test_relative_paths_resolve_against_cwd(self) -> None:
        out = "../core/src/a.h:2:2: warning: w [x-y]"
        build = self.root / "build"
        build.mkdir()
        diags, _ = ratchet.parse_diagnostics(out, self.root, build)
        self.assertEqual(diags, {("core/src/a.h", 2, 2, "x-y")})


class UncitedNolints(unittest.TestCase):
    def test_counts_only_uncited(self) -> None:
        text = "\n".join(
            [
                "int a; // NOLINT",
                "// ADR-0138 bit-exact per-lane reduction",
                "int b; // NOLINTNEXTLINE(readability-function-size)",
                "int c; // NOLINT(cert-dcl37-c) ADR-0278",
                "int e;",
                "// NOLINTBEGIN(bugprone-macro-parentheses)",
                "int d;",
                "// NOLINTEND",
            ]
        )
        self.assertEqual(ratchet.count_uncited_nolints(text), 2)

    def test_no_markers(self) -> None:
        self.assertEqual(ratchet.count_uncited_nolints("int x;\n"), 0)


class Compare(unittest.TestCase):
    def _m(self, warnings: dict, nolint: dict | None = None) -> ratchet.Measurement:
        return ratchet.Measurement(lane="cpu", warnings=warnings, nolint_uncited=nolint or {})

    def test_regression_and_slack(self) -> None:
        base = self._m({"a.c": 3, "b.c": 2}, {"a.c": 1})
        now = self._m({"a.c": 4, "b.c": 1, "c.c": 1}, {})
        regressions, slack = ratchet.compare(base, now)
        self.assertEqual(
            [(d.path, d.metric, d.change) for d in regressions],
            [("a.c", "warnings", 1), ("c.c", "warnings", 1)],
        )
        self.assertEqual(
            [(d.path, d.metric, d.change) for d in slack],
            [("b.c", "warnings", -1), ("a.c", "nolint_uncited", -1)],
        )

    def test_exit_codes(self) -> None:
        base = self._m({"a.c": 3})
        self.assertEqual(ratchet.report(base, self._m({"a.c": 3}), False), 0)
        self.assertEqual(ratchet.report(base, self._m({"a.c": 5}), False), 2)
        self.assertEqual(ratchet.report(base, self._m({"a.c": 1}), False), 3)
        self.assertEqual(ratchet.report(base, self._m({"a.c": 1}), True), 0)

    def test_baseline_round_trip(self) -> None:
        m = self._m({"b.c": 1, "a.c": 2}, {"a.c": 1})
        m.tus = 2
        data = json.loads(json.dumps(m.to_json()))
        back = ratchet.Measurement.from_json(data)
        self.assertEqual(back.warnings, {"a.c": 2, "b.c": 1})
        self.assertEqual(back.nolint_uncited, {"a.c": 1})
        self.assertEqual(back.tus, 2)
        self.assertEqual(list(data["warnings"]), ["a.c", "b.c"])

    def test_schema_mismatch_rejected(self) -> None:
        with self.assertRaises(ValueError):
            ratchet.Measurement.from_json({"schema": 99})


class CompileCommands(unittest.TestCase):
    def test_filters_to_in_repo_sources(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "core" / "src").mkdir(parents=True)
            (root / "subprojects" / "x").mkdir(parents=True)
            build = root / "build"
            build.mkdir()
            entries = [
                {"directory": str(build), "file": "../core/src/a.c", "command": "cc"},
                {"directory": str(build), "file": str(root / "core/src/a.c"), "command": "cc"},
                {"directory": str(build), "file": str(root / "subprojects/x/y.c"), "command": "cc"},
                {"directory": str(build), "file": "/usr/src/z.c", "command": "cc"},
                {"directory": str(build), "file": "../core/src/a.h", "command": "cc"},
            ]
            (build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
            units = ratchet.load_compile_commands(build, root)
            self.assertEqual([u[0].relative_to(root).as_posix() for u in units], ["core/src/a.c"])


if __name__ == "__main__":
    unittest.main()
