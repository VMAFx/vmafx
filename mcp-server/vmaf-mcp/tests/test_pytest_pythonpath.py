# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify pytest discovers vmaf_mcp via pythonpath configuration without editable install.

Landed in commit 993c0ef81 (PR #1559), clobbered in 384d97d03, and restored under
BUG-048 item A12.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import tomllib

_MCP_ROOT = Path(__file__).resolve().parents[1]


def test_vmaf_mcp_importable_without_editable_install() -> None:
    """vmaf_mcp package must be importable via pytest pythonpath = ['src']."""
    import vmaf_mcp

    assert hasattr(vmaf_mcp, "__version__")
    assert bool(vmaf_mcp.__version__)


def test_mcp_pyproject_toml_declares_src_pythonpath() -> None:
    """mcp-server/vmaf-mcp/pyproject.toml must declare pythonpath = ['src']."""
    pyproject = _MCP_ROOT / "pyproject.toml"
    assert pyproject.is_file(), f"{pyproject} not found"
    data = tomllib.loads(pyproject.read_text(encoding="utf-8"))
    pytest_opts = data.get("tool", {}).get("pytest", {}).get("ini_options", {})
    pythonpath = pytest_opts.get("pythonpath")
    assert pythonpath is not None, "tool.pytest.ini_options.pythonpath missing in pyproject.toml"
    assert "src" in pythonpath, f"'src' not in pythonpath: {pythonpath!r}"


def test_pythonpath_red_demonstration(tmp_path: Path) -> None:
    """Demonstrate that omitting pythonpath causes ModuleNotFoundError under clean subprocess."""
    clean_env = {
        "PATH": "/usr/bin:/bin",
        "HOME": str(tmp_path),
        "PYTHONNOUSERSITE": "1",
    }
    cmd_fail = [
        sys.executable,
        "-c",
        "import sys; sys.path = [p for p in sys.path if 'src' not in p]; import vmaf_mcp",
    ]
    proc_fail = subprocess.run(cmd_fail, env=clean_env, capture_output=True, text=True)
    assert proc_fail.returncode != 0
    assert "No module named 'vmaf_mcp'" in proc_fail.stderr

    cmd_pass = [
        sys.executable,
        "-c",
        f"import sys; sys.path.insert(0, '{_MCP_ROOT / 'src'}'); import vmaf_mcp; assert vmaf_mcp.__version__",
    ]
    proc_pass = subprocess.run(cmd_pass, env=clean_env, capture_output=True, text=True)
    assert proc_pass.returncode == 0
