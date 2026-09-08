#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Require positive file-selection fixtures for Renovate custom managers.

The repository uses anchored, flag-free regexes with the /regex/ delimiter
contract verified against Renovate 44.56.3's installed matcher. These patterns
use syntax shared by Python re and RE2; no Renovate installation is needed for
the local/CI guard. See docs/research/renovate-file-pattern-delimiters.md.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path
from typing import Any, ClassVar

ROOT = Path(__file__).resolve().parents[3]
GIT = shutil.which("git") or "/usr/bin/git"
BASE_FILES = {
    "build-config.env",
    "Dockerfile",
    "Dockerfile.go-server",
    "dev/Containerfile",
    "docker/Dockerfile.controller",
    "docker/Dockerfile.node",
    "docker/Dockerfile.operator",
    "docker/Dockerfile.production",
    "docker/Dockerfile.production-gpu",
}


class RenovateFilePatterns(unittest.TestCase):
    config: ClassVar[dict[str, Any]]
    files: ClassVar[list[str]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.config = json.loads((ROOT / "renovate.json").read_text(encoding="utf-8"))
        environment = {
            key: value for key, value in os.environ.items() if not key.startswith("GIT_")
        }
        cls.files = subprocess.check_output(  # noqa: S603 -- fixed read-only Git command
            [GIT, "-C", str(ROOT), "ls-files"], env=environment, text=True
        ).splitlines()

    def patterns(self, manager: dict[str, Any]) -> list[re.Pattern[str]]:
        patterns = manager["managerFilePatterns"]
        self.assertTrue(patterns)
        compiled = []
        for pattern in patterns:
            self.assertTrue(pattern.startswith("/") and pattern.endswith("/"), pattern)
            compiled.append(re.compile(pattern[1:-1]))
        return compiled

    def test_every_custom_pattern_selects_a_tracked_input(self) -> None:
        for manager in self.config["customManagers"]:
            name = manager.get("depNameTemplate", manager.get("description", "custom"))
            for pattern in self.patterns(manager):
                with self.subTest(manager=name, pattern=pattern.pattern):
                    selected = [path for path in self.files if pattern.search(path)]
                    self.assertTrue(selected, "Pattern selects no tracked files")
                    for path in selected:
                        self.assertIsNone(pattern.search(f".workingdir2/archive/{path}"))
                        self.assertIsNone(pattern.search(f"{path}.bak"))

    def test_base_image_manager_selects_config_and_all_mirrors(self) -> None:
        managers = [
            manager
            for manager in self.config["customManagers"]
            if manager["datasourceTemplate"] == "docker" and "depNameTemplate" not in manager
        ]
        self.assertEqual(len(managers), 1)
        patterns = self.patterns(managers[0])
        selected = {
            path for path in self.files if any(pattern.search(path) for pattern in patterns)
        }
        self.assertEqual(selected, BASE_FILES)

    def test_builtin_docker_exclusions_are_covered_by_the_custom_manager(self) -> None:
        rules = [
            rule
            for rule in self.config["packageRules"]
            if rule.get("matchManagers") == ["dockerfile"] and rule.get("enabled") is False
        ]
        self.assertEqual(len(rules), 1)
        self.assertEqual(set(rules[0]["matchFileNames"]), BASE_FILES - {"build-config.env"})


if __name__ == "__main__":
    unittest.main()
