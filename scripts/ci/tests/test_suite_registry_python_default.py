#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Suite installs inherit PYTHON_CI_VERSION unless they override it (issue 3103)."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import suite_registry  # noqa: E402

CHECKER_PATH = ROOT / "scripts/ci/check-workflow-versions.py"
spec = importlib.util.spec_from_file_location("check_workflow_versions", CHECKER_PATH)
assert spec is not None and spec.loader is not None
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class RegistryDefault(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="suite-python-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        (self.root / ".github").mkdir()
        (self.root / "build-config.env").write_text(
            'PYTHON_CI_VERSION="3.14.10"\n', encoding="utf-8"
        )

    def manifest(self, install: dict[str, object], name: str = "pkg") -> None:
        data = {
            "schema_version": 1,
            "name_patterns": ["test_*.py"],
            "suites": [
                {
                    "name": name,
                    "paths": ["pkg/"],
                    "checks": ["Unit"],
                    "install": install,
                }
            ],
        }
        (self.root / ".github" / "test-suites.json").write_text(json.dumps(data), encoding="utf-8")

    def test_install_without_python_inherits_the_config(self) -> None:
        self.manifest({"locks": ["lock.txt"]})
        registry = suite_registry.load_registry(self.root)
        self.assertEqual(registry.suites[0].install.python, "3.14")  # type: ignore[union-attr]

    def test_explicit_python_wins(self) -> None:
        self.manifest({"python": "3.13", "locks": ["lock.txt"]})
        registry = suite_registry.load_registry(self.root)
        self.assertEqual(registry.suites[0].install.python, "3.13")  # type: ignore[union-attr]

    def test_the_config_is_read_once_for_every_install(self) -> None:
        self.manifest({"locks": ["lock.txt"]})
        with mock.patch.object(
            suite_registry, "default_python", wraps=suite_registry.default_python
        ) as default:
            suite_registry.load_registry(self.root)
        self.assertEqual(default.call_count, 1)


class CheckerDefault(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="suite-python-check-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        (self.root / ".github").mkdir()

    def write(self, suites: list[dict[str, object]]) -> None:
        (self.root / ".github" / "test-suites.json").write_text(
            json.dumps({"suites": suites}), encoding="utf-8"
        )

    def test_inherited_default_is_clean(self) -> None:
        self.write([{"name": "pkg", "install": {"locks": ["lock.txt"]}}])
        self.assertEqual(checker.check_suite_registry_python(self.root, "3.14.10"), [])

    def test_repeating_the_default_is_refused(self) -> None:
        for repeated in ("3.14", "3.14.10"):
            with self.subTest(repeated=repeated):
                self.write([{"name": "pkg", "install": {"python": repeated, "locks": ["x"]}}])
                problems = checker.check_suite_registry_python(self.root, "3.14.10")
                self.assertIn("repeats install.python", problems[0])

    def test_an_explicit_override_is_allowed(self) -> None:
        self.write(
            [
                {"name": "pkg", "install": {"locks": ["x"]}},
                {"name": "legacy", "install": {"python": "3.13", "locks": ["y"]}},
            ]
        )
        self.assertEqual(checker.check_suite_registry_python(self.root, "3.14.10"), [])

    def test_no_inheriting_suite_is_refused(self) -> None:
        self.write([{"name": "pkg", "install": {"python": "3.13", "locks": ["x"]}}])
        problems = checker.check_suite_registry_python(self.root, "3.14.10")
        self.assertIn("no suite install inherits", problems[0])


if __name__ == "__main__":
    unittest.main()
