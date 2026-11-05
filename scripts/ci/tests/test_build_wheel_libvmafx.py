#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/build-wheel-libvmafx.py tool validation and asm fallback."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from types import ModuleType
from typing import Any
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[3]
BUILD_WHEEL_SCRIPT = ROOT / "scripts/ci/build-wheel-libvmafx.py"


def load_module() -> ModuleType:
    spec = importlib.util.spec_from_file_location("build_wheel_libvmafx", BUILD_WHEEL_SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BuildWheelLibvmafxTests(unittest.TestCase):
    mod: Any

    @classmethod
    def setUpClass(cls) -> None:
        cls.mod = load_module()

    def test_repo_root_resolves_to_existing_dir(self) -> None:
        root = self.mod.repo_root()
        self.assertTrue(root.is_dir())
        self.assertTrue((root / "core").is_dir())

    def test_ensure_build_tools_returns_resolved_executables(self) -> None:
        meson, ninja = self.mod.ensure_build_tools()
        self.assertTrue(Path(meson).is_file())
        self.assertTrue(Path(ninja).is_file())
        self.assertTrue(Path(meson).is_absolute())
        self.assertTrue(Path(ninja).is_absolute())

    def test_ensure_build_tools_missing_lock_raises_filenotfound(self) -> None:
        """Planted defect: missing requirements/locks/build.txt must fail fast."""
        with patch.object(self.mod, "repo_root", return_value=ROOT / "nonexistent"):
            with patch("shutil.which", return_value=None):
                with self.assertRaises(FileNotFoundError) as ctx:
                    self.mod.ensure_build_tools()
                self.assertIn("Expected repository build lock file not found", str(ctx.exception))

    def test_ensure_build_tools_unresolved_tools_raises_runtime_error(self) -> None:
        """Planted defect: if tools cannot be located after pip install, fail without unqualified fallback."""
        with patch("shutil.which", return_value=None):
            with patch("subprocess.run") as mock_run:
                mock_run.return_value = MagicMock(returncode=0)
                with self.assertRaises(RuntimeError) as ctx:
                    self.mod.ensure_build_tools()
                self.assertIn("Failed to resolve meson or ninja executable", str(ctx.exception))

    def test_setup_meson_disables_asm_when_nasm_missing(self) -> None:
        """When nasm is not available in PATH, -Denable_asm=false must be added to meson flags."""
        captured_cmd: list[str] = []

        def fake_run(cmd: list[str], **kwargs: Any) -> MagicMock:
            captured_cmd.extend(cmd)
            return MagicMock(returncode=0)

        with patch("shutil.which", return_value=None):
            with patch("subprocess.run", side_effect=fake_run):
                self.mod.setup_meson(ROOT, "meson")

        self.assertIn("-Denable_asm=false", captured_cmd)

    def test_setup_meson_retains_asm_when_nasm_present(self) -> None:
        """When nasm is available, -Denable_asm=false is not appended."""
        captured_cmd: list[str] = []

        def fake_run(cmd: list[str], **kwargs: Any) -> MagicMock:
            captured_cmd.extend(cmd)
            return MagicMock(returncode=0)

        def mock_which(cmd: str) -> str | None:
            if cmd == "nasm":
                return "/usr/bin/nasm"
            return None

        with patch("shutil.which", side_effect=mock_which):
            with patch("subprocess.run", side_effect=fake_run):
                self.mod.setup_meson(ROOT, "meson")

        self.assertNotIn("-Denable_asm=false", captured_cmd)


if __name__ == "__main__":
    unittest.main()
