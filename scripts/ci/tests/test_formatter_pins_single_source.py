#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify the Makefile installs ruff and black at the versions pre-commit runs."""

from __future__ import annotations

import importlib.util
import tempfile
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


class FormatterGate(Protocol):
    def check_formatter_pins(self, root: Path) -> list[str]: ...

    def check_makefile_venv_paths(self, root: Path) -> list[str]: ...

    def precommit_rev(self, text: str, repo: str) -> str | None: ...


GATE = cast(FormatterGate, MODULE)

HOOKS = """repos:
  - repo: https://github.com/psf/black
    rev: 26.5.1
    hooks:
      - id: black
  - repo: https://github.com/astral-sh/ruff-pre-commit
    rev: v0.16.8
    hooks:
      - id: ruff-check
"""

MAKEFILE = """RUFF_VERSION  := 0.16.8
BLACK_VERSION := 26.5.1

lint-tools:
\tpip install 'ruff==$(RUFF_VERSION)' 'black==$(BLACK_VERSION)'
"""

VENV_MAKEFILE = """VENV := .venv
VIRTUAL_ENV_PATH := $(abspath $(VENV))/bin
VENV_PYTHON := $(VIRTUAL_ENV_PATH)/python

build:
\tPATH="$(VIRTUAL_ENV_PATH):$$PATH" ninja -C core/build

cythonize:
\tcd python && $(VENV_PYTHON) setup.py build_ext --build-lib .
"""


class FormatterPinsSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)

    def check(self, makefile: str, hooks: str = HOOKS) -> list[str]:
        (self.repo / "Makefile").write_text(makefile, encoding="utf-8")
        (self.repo / ".pre-commit-config.yaml").write_text(hooks, encoding="utf-8")
        return GATE.check_formatter_pins(self.repo)

    def check_venv_paths(self, makefile: str) -> list[str]:
        (self.repo / "Makefile").write_text(makefile, encoding="utf-8")
        return GATE.check_makefile_venv_paths(self.repo)

    def test_repository_is_in_sync(self) -> None:
        self.assertEqual(GATE.check_formatter_pins(ROOT), [])

    def test_matching_pins_pass(self) -> None:
        self.assertEqual(self.check(MAKEFILE), [])

    def test_makefile_behind_the_hook_fails(self) -> None:
        problems = self.check(MAKEFILE.replace("0.16.8", "0.16.5"))
        self.assertEqual(len(problems), 1)
        self.assertIn("RUFF_VERSION := 0.16.5", problems[0])
        self.assertIn("ruff 0.16.8", problems[0])

    def test_hook_behind_the_makefile_fails(self) -> None:
        problems = self.check(MAKEFILE, HOOKS.replace("rev: 26.5.1", "rev: 26.4.0"))
        self.assertEqual(len(problems), 1)
        self.assertIn("BLACK_VERSION", problems[0])

    def test_literal_version_in_a_recipe_fails(self) -> None:
        problems = self.check(MAKEFILE + "\nlint-py:\n\tpip install ruff==0.15.17\n")
        self.assertEqual(len(problems), 1)
        self.assertIn("ruff==0.15.17", problems[0])
        self.assertIn("$(RUFF_VERSION)", problems[0])

    def test_rev_is_read_with_and_without_the_v_prefix(self) -> None:
        self.assertEqual(
            GATE.precommit_rev(HOOKS, "https://github.com/astral-sh/ruff-pre-commit"), "0.16.8"
        )
        self.assertEqual(GATE.precommit_rev(HOOKS, "https://github.com/psf/black"), "26.5.1")
        self.assertIsNone(GATE.precommit_rev(HOOKS, "https://example.invalid/none"))

    def test_missing_files_are_not_a_finding(self) -> None:
        self.assertEqual(GATE.check_formatter_pins(self.repo), [])

    def test_repository_venv_paths_are_absolute(self) -> None:
        self.assertEqual(GATE.check_makefile_venv_paths(ROOT), [])

    def test_absolute_venv_paths_pass(self) -> None:
        self.assertEqual(self.check_venv_paths(VENV_MAKEFILE), [])

    def test_absolute_alias_spelling_passes(self) -> None:
        """The alias spelling this repository uses satisfies the same property.

        `VIRTUAL_ENV_ABS := $(abspath $(VIRTUAL_ENV_PATH))` makes the path
        absolute in two steps instead of one. The gate checks that the
        Meson-facing path IS absolute, not which name holds it.
        """
        alias = VENV_MAKEFILE.replace(
            "VIRTUAL_ENV_PATH := $(abspath $(VENV))/bin",
            "VIRTUAL_ENV_PATH := $(VENV)/bin\nVIRTUAL_ENV_ABS := $(abspath $(VIRTUAL_ENV_PATH))",
        ).replace("$(VIRTUAL_ENV_PATH):$$PATH", "$(VIRTUAL_ENV_ABS):$$PATH")
        self.assertEqual(self.check_venv_paths(alias), [])

    def test_neither_absolute_spelling_fails(self) -> None:
        neither = VENV_MAKEFILE.replace(
            "VIRTUAL_ENV_PATH := $(abspath $(VENV))/bin", "VIRTUAL_ENV_PATH := $(VENV)/bin"
        )
        problems = self.check_venv_paths(neither)
        self.assertEqual(len(problems), 1)
        self.assertIn("absolute project-venv bin path", problems[0])

    def test_relative_meson_tool_path_fails(self) -> None:
        relative = VENV_MAKEFILE.replace("$(abspath $(VENV))/bin", "$(VENV)/bin").replace(
            "$(VIRTUAL_ENV_PATH):$$PATH", "$(VENV)/bin:$$PATH"
        )
        problems = self.check_venv_paths(relative)
        self.assertEqual(len(problems), 2)
        self.assertIn("VIRTUAL_ENV_PATH", problems[0])
        self.assertIn("relative $(VENV)/bin", problems[1])

    def test_relative_python_after_chdir_fails(self) -> None:
        problems = self.check_venv_paths(
            VENV_MAKEFILE.replace("$(VENV_PYTHON) setup.py", "../$(VENV_PYTHON) setup.py")
        )
        self.assertEqual(len(problems), 1)
        self.assertIn("absolute $(VENV_PYTHON)", problems[0])


if __name__ == "__main__":
    unittest.main()
