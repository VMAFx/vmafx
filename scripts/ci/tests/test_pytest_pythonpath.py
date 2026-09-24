#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract test for pytest pythonpath configuration across sub-packages.

Guards against silent reversion of BUG-048 item A12 (commit 993c0ef81 / PR #1559,
clobbered by 384d97d03), where mcp-server/vmaf-mcp/pyproject.toml lost
`pythonpath = ["src"]`, causing all tests in mcp-server/vmaf-mcp/tests/ to crash
at collection with `ModuleNotFoundError: No module named 'vmaf_mcp'` when run
without an editable pip install.

Run with:  python3 -m unittest scripts/ci/tests/test_pytest_pythonpath.py
No third-party dependencies (uses stdlib tomllib).
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import tomllib

REPO_ROOT = Path(__file__).resolve().parents[3]


class PytestPythonpathContract(unittest.TestCase):
    def test_mcp_pyproject_declares_src_pythonpath(self) -> None:
        """mcp-server/vmaf-mcp/pyproject.toml must configure pythonpath = ['src']."""
        mcp_toml = REPO_ROOT / "mcp-server" / "vmaf-mcp" / "pyproject.toml"
        self.assertTrue(mcp_toml.is_file(), f"{mcp_toml} does not exist")

        data = tomllib.loads(mcp_toml.read_text(encoding="utf-8"))
        pytest_opts = data.get("tool", {}).get("pytest", {}).get("ini_options", {})
        pythonpath = pytest_opts.get("pythonpath")

        self.assertIsNotNone(
            pythonpath,
            f"{mcp_toml}: [tool.pytest.ini_options] missing pythonpath configuration",
        )
        self.assertIn(
            "src",
            pythonpath,
            f"{mcp_toml}: pythonpath must contain 'src' to locate vmaf_mcp without editable install",
        )

    def test_mcp_package_source_layout_aligns_with_pythonpath(self) -> None:
        """The source root named in pythonpath must contain the package init file."""
        pkg_init = REPO_ROOT / "mcp-server" / "vmaf-mcp" / "src" / "vmaf_mcp" / "__init__.py"
        self.assertTrue(
            pkg_init.is_file(),
            f"Expected package init at {pkg_init} for pythonpath = ['src'] discovery",
        )

    def test_missing_pythonpath_red_demonstration(self) -> None:
        """Subprocess without src in sys.path must fail to import vmaf_mcp."""
        with tempfile.TemporaryDirectory() as temp_dir:
            neutral_site = Path(temp_dir)
            neutral_package = neutral_site / "vmaf_mcp"
            neutral_package.mkdir()
            (neutral_package / "__init__.py").write_text(
                '__version__ = "neutral-wheel"\n', encoding="utf-8"
            )
            child_env = os.environ.copy()
            child_env["PYTHONPATH"] = str(neutral_site)
            cmd = [
                sys.executable,
                "-I",
                "-S",
                "-c",
                "import vmaf_mcp",
            ]
            proc = subprocess.run(  # noqa: S603 -- fixed argv test fixture
                cmd, env=child_env, capture_output=True, text=True
            )
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("ModuleNotFoundError", proc.stderr)
            self.assertIn("vmaf_mcp", proc.stderr)

    def test_present_pythonpath_import_success(self) -> None:
        """Subprocess with src prepended to sys.path must import vmaf_mcp successfully."""
        src_path = REPO_ROOT / "mcp-server" / "vmaf-mcp" / "src"
        src_path_literal = repr(str(src_path))
        expected_init_literal = repr(str(src_path / "vmaf_mcp" / "__init__.py"))
        cmd = [
            sys.executable,
            "-I",
            "-S",
            "-c",
            (
                "import sys; from pathlib import Path; "
                f"sys.path.insert(0, {src_path_literal}); "
                "import vmaf_mcp; "
                f"assert Path(vmaf_mcp.__file__).resolve() == Path({expected_init_literal}).resolve(); "
                "assert bool(vmaf_mcp.__version__)"
            ),
        ]
        child_env = os.environ.copy()
        proc = subprocess.run(  # noqa: S603 -- fixed argv test fixture
            cmd, env=child_env, capture_output=True, text=True
        )
        self.assertEqual(proc.returncode, 0, f"Import failed: {proc.stderr}")

    def test_root_and_ai_pyproject_pythonpath_consistency(self) -> None:
        """Root and AI pyproject.toml files must retain their declared pythonpaths."""
        root_toml = REPO_ROOT / "pyproject.toml"
        self.assertTrue(root_toml.is_file())
        root_data = tomllib.loads(root_toml.read_text(encoding="utf-8"))
        root_path = (
            root_data.get("tool", {}).get("pytest", {}).get("ini_options", {}).get("pythonpath")
        )
        self.assertIsNotNone(root_path)
        self.assertIn("ai/src", root_path)

        ai_toml = REPO_ROOT / "ai" / "pyproject.toml"
        self.assertTrue(ai_toml.is_file())
        ai_data = tomllib.loads(ai_toml.read_text(encoding="utf-8"))
        ai_path = ai_data.get("tool", {}).get("pytest", {}).get("ini_options", {}).get("pythonpath")
        self.assertIsNotNone(ai_path)
        self.assertIn("src", ai_path)


if __name__ == "__main__":
    unittest.main()
