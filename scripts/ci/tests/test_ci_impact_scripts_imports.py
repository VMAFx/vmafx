#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin Build workflow routing for transitive scripts imports from Python tests."""

from __future__ import annotations

import ast
import copy
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci.tests.test_ci_impact import CONFIG, REPO_ROOT, _plan_for, planner

# Bound on module names followed from the Python test trees, including parents.
MAX_SCRIPTS_IMPORT_MODULES = 256


def _scripts_imports(path: Path) -> set[str]:
    """Return absolute scripts module imports and possible imported submodules."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    found: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            if node.module.split(".")[0] == "scripts":
                found.add(node.module)
                found.update(f"{node.module}.{alias.name}" for alias in node.names)
        elif isinstance(node, ast.Import):
            found.update(a.name for a in node.names if a.name.split(".")[0] == "scripts")
    return found


def _module_file(module: str, *, repo_root: Path = REPO_ROOT) -> Path | None:
    """Locate a module or package source under the injected repository root."""
    base = repo_root.joinpath(*module.split("."))
    for candidate in (base.with_suffix(".py"), base / "__init__.py"):
        if candidate.is_file():
            return candidate
    return None


def _scripts_imports_from_roots(roots: tuple[str, ...], repo_root: Path) -> set[str]:
    """Seed module discovery from Python sources outside tox environments."""
    pending: set[str] = set()
    for root in roots:
        for source in (repo_root / root).rglob("*.py"):
            if ".tox" not in source.parts:
                pending |= _scripts_imports(source)
    return pending


def _scripts_modules_imported_from(
    roots: tuple[str, ...], *, repo_root: Path = REPO_ROOT
) -> list[str]:
    """Follow scripts imports and parent initializers within the named module bound."""
    pending = _scripts_imports_from_roots(roots, repo_root)
    seen: set[str] = set()
    files: set[Path] = set()
    for _ in range(MAX_SCRIPTS_IMPORT_MODULES):
        if not pending:
            break
        module = pending.pop()
        seen.add(module)
        parts = module.split(".")
        pending.update(".".join(parts[:length]) for length in range(1, len(parts)))
        source_file = _module_file(module, repo_root=repo_root)
        if source_file is not None and source_file not in files:
            files.add(source_file)
            pending |= _scripts_imports(source_file)
        pending -= seen
    else:
        if pending:
            raise AssertionError(
                f"scripts imports exceed MAX_SCRIPTS_IMPORT_MODULES={MAX_SCRIPTS_IMPORT_MODULES}"
            )
    return sorted(f.relative_to(repo_root).as_posix() for f in files)


class ScriptsImportRouting(unittest.TestCase):
    """Require build_work to select every scripts dependency of the Python test trees."""

    def _assert_scripts_routed(self, config: dict[str, object]) -> None:
        """Name any imported scripts source omitted by an injected build selector."""
        for path in _scripts_modules_imported_from(("python", "compat")):
            self.assertTrue(_plan_for([path], config=config).selectors["build_work"], path)

    def test_every_scripts_module_the_python_tests_import_selects_it(self) -> None:
        """Route shared helpers and parent initializers through the real build selector."""
        modules = _scripts_modules_imported_from(("python", "compat"))
        for path in (
            "scripts/__init__.py",
            "scripts/lib/__init__.py",
            "scripts/lib/safe_subprocess.py",
        ):
            with self.subTest(path=path):
                self.assertIn(path, modules)
        self._assert_scripts_routed(planner.load_config(CONFIG))

    def test_planted_selector_missing_an_import_names_the_file(self) -> None:
        """Detect missing parent initializers and helpers through the same routing check."""
        for missing in ("scripts/__init__.py", "scripts/lib/safe_subprocess.py"):
            with self.subTest(missing=missing):
                config = copy.deepcopy(planner.load_config(CONFIG))
                config["selectors"]["build_work"]["patterns"].remove(missing)
                with self.assertRaises(AssertionError) as raised:
                    self._assert_scripts_routed(config)
                self.assertIn(missing, str(raised.exception))


class ScriptsImportGraph(unittest.TestCase):
    """Exercise package traversal and module bounds against disposable source trees."""

    def _graph(self, repo_root: Path, count: int) -> list[str]:
        """Write a chain containing exactly count modules, including the scripts package."""
        tests = repo_root / "tests"
        tests.mkdir()
        (tests / "launch_test.py").write_text("import scripts.module_0\n", encoding="utf-8")
        package = repo_root / "scripts"
        package.mkdir()
        (package / "__init__.py").write_text("", encoding="utf-8")
        for number in range(count - 1):
            text = f"import scripts.module_{number + 1}\n" if number < count - 2 else ""
            (package / f"module_{number}.py").write_text(text, encoding="utf-8")
        return ["scripts/__init__.py"] + [
            f"scripts/module_{number}.py" for number in range(count - 1)
        ]

    def test_exact_module_bound_is_accepted(self) -> None:
        """Accept a graph that finishes on the last permitted module."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = self._graph(root, MAX_SCRIPTS_IMPORT_MODULES)
            self.assertEqual(
                _scripts_modules_imported_from(("tests",), repo_root=root), sorted(expected)
            )

    def test_one_module_over_bound_is_refused(self) -> None:
        """Reject a graph with work remaining after the last permitted module."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._graph(root, MAX_SCRIPTS_IMPORT_MODULES + 1)
            with self.assertRaisesRegex(
                AssertionError, f"MAX_SCRIPTS_IMPORT_MODULES={MAX_SCRIPTS_IMPORT_MODULES}"
            ):
                _scripts_modules_imported_from(("tests",), repo_root=root)

    def test_parent_initializers_imports_are_followed(self) -> None:
        """Discover dependencies imported only by parent package initializers."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._graph(root, 3)
            package = root / "scripts"
            (package / "__init__.py").write_text("import scripts.extra\n", encoding="utf-8")
            (package / "extra.py").write_text("", encoding="utf-8")
            library = package / "lib"
            library.mkdir()
            (library / "__init__.py").write_text("import scripts.other\n", encoding="utf-8")
            (library / "helper.py").write_text("", encoding="utf-8")
            (package / "other.py").write_text("", encoding="utf-8")
            (root / "tests" / "launch_test.py").write_text(
                "from scripts.lib import helper\n", encoding="utf-8"
            )
            self.assertEqual(
                _scripts_modules_imported_from(("tests",), repo_root=root),
                [
                    "scripts/__init__.py",
                    "scripts/extra.py",
                    "scripts/lib/__init__.py",
                    "scripts/lib/helper.py",
                    "scripts/other.py",
                ],
            )

    def test_tree_without_scripts_imports_is_empty(self) -> None:
        """Return no dependencies when the source tree imports no scripts modules."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "launch_test.py").write_text("import pathlib\n", encoding="utf-8")
            self.assertEqual(_scripts_modules_imported_from((".",), repo_root=root), [])


if __name__ == "__main__":
    unittest.main()
