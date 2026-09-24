# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify pytest discovers vmaf_mcp via pythonpath configuration without editable install.

Landed in commit 993c0ef81 (PR #1559), clobbered in 384d97d03, and restored under
BUG-048 item A12.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import tomllib

_MCP_ROOT = Path(__file__).resolve().parents[1]


def test_vmaf_mcp_importable_without_editable_install() -> None:
    """vmaf_mcp package must be importable via pytest pythonpath = ['src']."""
    import vmaf_mcp

    expected_init = (_MCP_ROOT / "src" / "vmaf_mcp" / "__init__.py").resolve()
    assert Path(vmaf_mcp.__file__).resolve() == expected_init
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
    """Only an explicit source path may satisfy the isolated child import."""
    neutral_site = tmp_path / "neutral-site"
    neutral_package = neutral_site / "vmaf_mcp"
    neutral_package.mkdir(parents=True)
    (neutral_package / "__init__.py").write_text(
        '__version__ = "neutral-wheel"\n', encoding="utf-8"
    )
    child_env = os.environ.copy()
    child_env["PYTHONPATH"] = str(neutral_site)
    cmd_fail = [
        sys.executable,
        "-I",
        "-S",
        "-c",
        "import vmaf_mcp",
    ]
    proc_fail = subprocess.run(cmd_fail, env=child_env, capture_output=True, text=True)
    assert proc_fail.returncode != 0
    assert "ModuleNotFoundError" in proc_fail.stderr
    assert "vmaf_mcp" in proc_fail.stderr

    src_path = _MCP_ROOT / "src"
    src_path_literal = repr(str(src_path))
    expected_init_literal = repr(str(src_path / "vmaf_mcp" / "__init__.py"))
    cmd_pass = [
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
    proc_pass = subprocess.run(cmd_pass, env=child_env, capture_output=True, text=True)
    assert proc_pass.returncode == 0, proc_pass.stderr
