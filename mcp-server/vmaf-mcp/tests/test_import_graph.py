# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Import-boundary regressions for the production ``vmaf_mcp`` package."""

from __future__ import annotations

import ast
import os
import subprocess
import sys
from graphlib import CycleError, TopologicalSorter
from importlib.util import resolve_name
from pathlib import Path

import pytest


def _module_name(source: Path) -> str:
    return "vmaf_mcp" if source.stem == "__init__" else f"vmaf_mcp.{source.stem}"


def _package_imports(source: Path, modules: set[str]) -> set[str]:
    """Return production-package modules imported anywhere in *source*."""
    imports: set[str] = set()
    importer = _module_name(source)
    importer_package = importer if source.stem == "__init__" else importer.rpartition(".")[0]
    tree = ast.parse(source.read_text(encoding="utf-8"), filename=str(source))
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imports.update(alias.name for alias in node.names if alias.name in modules)
        elif isinstance(node, ast.ImportFrom):
            if node.level:
                relative = f"{'.' * node.level}{node.module or ''}"
                imported = resolve_name(relative, importer_package)
            else:
                imported = node.module or ""
            if imported in modules:
                imports.add(imported)
            imports.update(
                candidate
                for alias in node.names
                if (candidate := f"{imported}.{alias.name}") in modules
            )
    return imports


def test_production_module_import_graph_is_acyclic() -> None:
    """Every production module must be importable without circular ownership.

    Function-local imports still form static dependency edges and are reported
    by CodeQL's ``py/cyclic-import`` query.  Keeping the package graph acyclic
    preserves type checking and IDE navigation without hiding edges behind
    dynamic imports.
    """
    package_dir = Path(__file__).parents[1] / "src" / "vmaf_mcp"
    sources = sorted(package_dir.glob("*.py"))
    modules = {_module_name(source) for source in sources}
    graph = {_module_name(source): _package_imports(source, modules) for source in sources}

    try:
        tuple(TopologicalSorter(graph).static_order())
    except CycleError as exc:
        pytest.fail(f"production import cycle: {exc.args[1]}")


def test_http_transport_import_isolated_from_server_bootstrap() -> None:
    """The HTTP module loads alone; importing the canonical server installs scoring."""
    source_root = Path(__file__).parents[1] / "src"
    script = """
import sys

import vmaf_mcp.http_transport
from vmaf_mcp.http_scoring import get_http_scoring_runtime

assert "vmaf_mcp.server" not in sys.modules
try:
    get_http_scoring_runtime()
except RuntimeError:
    pass
else:
    raise AssertionError("HTTP scoring unexpectedly installed before server bootstrap")

try:
    vmaf_mcp.http_transport.run_http_server()
except RuntimeError:
    pass
else:
    raise AssertionError("standalone HTTP startup did not fail before binding")

import vmaf_mcp.server

get_http_scoring_runtime()
"""
    env = os.environ.copy()
    env["PYTHONPATH"] = str(source_root)
    completed = subprocess.run(
        [sys.executable, "-c", script],
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    assert completed.returncode == 0, completed.stderr


def test_server_import_preserves_preinstalled_http_runtime() -> None:
    """Embedding adapters survive a later import of the canonical server."""
    source_root = Path(__file__).parents[1] / "src"
    script = """
from pathlib import Path

from vmaf_mcp.http_scoring import get_http_scoring_runtime, install_http_scoring_runtime


class EmbeddedRuntime:
    def vmaf_binary(self):
        return Path("vmaf")

    def build_request(self, **fields):
        return fields

    async def run_score(self, request):
        return request

    def dumps_strict(self, data):
        return "{}"


runtime = EmbeddedRuntime()
install_http_scoring_runtime(runtime)
import vmaf_mcp.server

assert get_http_scoring_runtime() is runtime
"""
    env = os.environ.copy()
    env["PYTHONPATH"] = str(source_root)
    completed = subprocess.run(
        [sys.executable, "-c", script],
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    assert completed.returncode == 0, completed.stderr
