#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/check-torch-scope.py: torch only in the training packages (ADR-1886)."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ci/check-torch-scope.py"
_spec = importlib.util.spec_from_file_location("check_torch_scope", SCRIPT)
assert _spec is not None and _spec.loader is not None
scope = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(scope)

RUNTIME = """
[project]
name = "runtime"
dependencies = ["numpy>=2"]
[project.optional-dependencies]
dev = ["pytest"]
"""


def check(files: dict[str, str]) -> list[str]:
    found: list[str] = scope.violations(sorted(files), files.__getitem__)
    return found


class TorchScope(unittest.TestCase):
    def test_clean_runtime_package_passes(self) -> None:
        self.assertEqual(check({"tools/x/pyproject.toml": RUNTIME}), [])

    def test_training_roots_may_depend_on_torch(self) -> None:
        files = {
            "ai/pyproject.toml": '[project]\nname = "t"\ndependencies = ["torch>=2"]\n',
            "ai/requirements-dev-lock.txt": "torch==2.14.1 \\\n",
            "tools/ensemble-training-kit/pyproject.toml": '[project]\nname = "e"\ndependencies = ["torch"]\n',
        }
        self.assertEqual(check(files), [])

    def test_torch_in_an_optional_group_of_a_runtime_package_fails(self) -> None:
        text = RUNTIME + 'vlm = ["torch>=2.14.1"]\n'
        found = check({"mcp-server/vmaf-mcp/pyproject.toml": text})
        self.assertEqual(len(found), 1)
        self.assertIn("optional-dependencies.vlm names torch", found[0])

    def test_every_dependency_field_is_read(self) -> None:
        text = """
[project]
name = "r"
dependencies = ["TorchVision>=0.29"]
[dependency-groups]
dev = ["pytorch_lightning", {include-group = "x"}]
[build-system]
requires = ["torchao"]
"""
        found = check({"tools/r/pyproject.toml": text})
        self.assertEqual(
            [line.split(": ", 1)[1].split(" (")[0] for line in found],
            [
                "dependencies names torchvision",
                "dependency-groups.dev names pytorch-lightning",
                "build-system.requires names torchao",
            ],
        )

    def test_a_lock_inside_a_runtime_package_fails(self) -> None:
        files = {
            "tools/vmaf-tune/pyproject.toml": RUNTIME,
            "tools/vmaf-tune/requirements-dev-lock.txt": "numpy==2.5 \\\n    --hash=sha256:00\ntorch==2.14.1 \\\n",
        }
        found = check(files)
        self.assertEqual(found, ["tools/vmaf-tune/requirements-dev-lock.txt:3: resolves torch"])

    def test_module_lists_and_comments_are_not_dependencies(self) -> None:
        root = '[project]\nname = "tooling"\n[[tool.mypy.overrides]]\nmodule = ["torchao", "torch.*"]\n'
        files = {
            "pyproject.toml": root,
            "dev/requirements-python-env-lock.txt": "torch==2.14.1\n",
            "tools/x/pyproject.toml": RUNTIME,
            "tools/x/requirements.in": "# torch is not wanted here\n--hash=x\n",
        }
        self.assertEqual(check(files), [])

    def test_names_are_matched_whole(self) -> None:
        files = {"tools/x/pyproject.toml": RUNTIME.replace("numpy>=2", "torchserve-client")}
        self.assertEqual(check(files), [])

    def test_the_tree_passes(self) -> None:
        result = subprocess.run(  # noqa: S603 -- the script under test
            [sys.executable, str(SCRIPT)], capture_output=True, text=True, check=False
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
