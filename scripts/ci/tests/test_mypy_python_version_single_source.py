#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify mypy models the Python version `requires-python` floors the project at.

ADR-1282 raised `[tool.mypy] python_version` from `3.10` to `3.14`. The pin
survived a `requires-python` bump because a comment was the only thing holding
the two together, and a silent revert would be caught by nothing: mypy accepts
any version it knows about, and below 3.12 it refuses to parse the PEP 695
`type` statement in numpy's bundled `__init__.pyi`, so the `ai/src/` pass aborts
with "errors prevented further checking" rather than reporting anything that
names this setting. BUG-047 and BUG-048 were the same shape of silent revert.
"""

from __future__ import annotations

import importlib.util
import tempfile
import textwrap
import unittest
from pathlib import Path
from typing import Protocol, cast

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "workflow_versions", ROOT / "scripts/ci/check-workflow-versions.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class MypyVersionGate(Protocol):
    def check_mypy_python_version(self, root: Path, py_ci: str | None = None) -> list[str]: ...

    def requires_python_floor(self, spec: str) -> str | None: ...


GATE = cast(MypyVersionGate, MODULE)


def pyproject(requires: str = ">=3.14", pin: str = 'python_version = "3.14"') -> str:
    """A minimal pyproject.toml; `pin` is the whole `[tool.mypy]` pin line."""
    return textwrap.dedent(f"""\
        [project]
        name = "vmafx"
        requires-python = "{requires}"

        [tool.mypy]
        {pin}
        strict = true
        """)


class MypyPythonVersionSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)

    def check(self, text: str, py_ci: str | None = "3.14.7") -> list[str]:
        (self.repo / "pyproject.toml").write_text(text, encoding="utf-8")
        return GATE.check_mypy_python_version(self.repo, py_ci)

    def test_repository_is_in_sync(self) -> None:
        """The tracked pyproject.toml satisfies its own contract."""
        self.assertEqual(GATE.check_mypy_python_version(ROOT, "3.14.7"), [])

    def test_matching_versions_pass(self) -> None:
        self.assertEqual(self.check(pyproject()), [])

    def test_the_stale_pin_this_gate_exists_for_fails(self) -> None:
        """A silent revert to the ADR-1282 value is the regression being guarded."""
        problems = self.check(pyproject(pin='python_version = "3.10"'))
        self.assertEqual(len(problems), 1)
        self.assertIn("python_version = '3.10'", problems[0])
        self.assertIn(">=3.14", problems[0])
        self.assertIn("ADR-1282", problems[0])

    def test_raising_requires_python_alone_fails(self) -> None:
        """Drift is symmetric: moving the floor without the pin is the same bug."""
        problems = self.check(pyproject(requires=">=3.15"))
        self.assertEqual(len(problems), 1)
        self.assertIn("floors the project at 3.15", problems[0])

    def test_deleting_the_pin_fails(self) -> None:
        """ADR-1282 rejected letting mypy float with the running interpreter."""
        problems = self.check(pyproject(pin="# python_version intentionally absent"))
        self.assertEqual(len(problems), 1)
        self.assertIn("no python_version", problems[0])

    def test_an_unquoted_pin_fails(self) -> None:
        """TOML reads a bare 3.14 as a float; mypy requires a string."""
        problems = self.check(pyproject(pin="python_version = 3.14"))
        self.assertEqual(len(problems), 1)
        self.assertIn("must be a quoted string", problems[0])

    def test_ci_interpreter_must_agree(self) -> None:
        problems = self.check(pyproject(), py_ci="3.13.2")
        self.assertEqual(len(problems), 1)
        self.assertIn("PYTHON_CI_VERSION", problems[0])

    def test_ci_patch_level_is_not_a_finding(self) -> None:
        self.assertEqual(self.check(pyproject(), py_ci="3.14.0"), [])

    def test_a_requires_python_without_a_floor_fails_closed(self) -> None:
        problems = self.check(pyproject(requires="<4.0"))
        self.assertEqual(len(problems), 1)
        self.assertIn("no lower bound", problems[0])

    def test_floor_is_read_from_every_specifier_form(self) -> None:
        self.assertEqual(GATE.requires_python_floor(">=3.14"), "3.14")
        self.assertEqual(GATE.requires_python_floor(">=3.14,<4.0"), "3.14")
        self.assertEqual(GATE.requires_python_floor("~=3.14"), "3.14")
        self.assertEqual(GATE.requires_python_floor("==3.14.*"), "3.14")
        # A specifier set is a conjunction: the strictest lower bound wins.
        self.assertEqual(GATE.requires_python_floor(">=3.9,>=3.14"), "3.14")
        self.assertEqual(GATE.requires_python_floor(">=3.14,<=3.20"), "3.14")
        self.assertIsNone(GATE.requires_python_floor("<4.0"))

    def test_missing_files_are_not_a_finding(self) -> None:
        self.assertEqual(GATE.check_mypy_python_version(self.repo), [])

    def test_a_pyproject_without_a_mypy_table_is_not_a_finding(self) -> None:
        text = '[project]\nname = "x"\nrequires-python = ">=3.14"\n'
        self.assertEqual(self.check(text), [])

    def test_unparseable_toml_is_reported(self) -> None:
        problems = self.check("[project\n")
        self.assertEqual(len(problems), 1)
        self.assertIn("not valid TOML", problems[0])


if __name__ == "__main__":
    unittest.main()
