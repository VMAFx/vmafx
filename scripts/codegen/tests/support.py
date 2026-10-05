# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared helpers of the VMAFx API generator tests (ADR-1852)."""

from __future__ import annotations

import io
import shutil
import subprocess
import sys
from collections.abc import Callable
from contextlib import redirect_stdout
from pathlib import Path
from typing import Any

import tomllib

ROOT = Path(__file__).resolve().parents[3]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
sys.path.insert(0, str(ROOT / "scripts" / "codegen"))

from vmafx_api import cli  # noqa: E402 -- path set above
from vmafx_api.loader import parse  # noqa: E402
from vmafx_api.model import Api  # noqa: E402

DEFINITION = ROOT / "core" / "api" / "vmafx.toml"
TOOL_TIMEOUT = 120  # seconds for a compiler or linker run


def document(path: Path = DEFINITION) -> dict[str, Any]:
    with path.open("rb") as handle:
        return tomllib.load(handle)


def fixture() -> dict[str, Any]:
    """The every-feature test definition (fixtures/full.toml)."""
    return document(FIXTURES / "full.toml")


def entry(items: list[dict[str, Any]], name: str) -> dict[str, Any]:
    return next(item for item in items if item.get("name", item.get("path")) == name)


def bumped(doc: dict[str, Any], version: str) -> dict[str, Any]:
    doc["api"]["abi_version"] = version
    return doc


def quiet(function: Callable[..., int], *args: object) -> int:
    with redirect_stdout(io.StringIO()):
        return function(*args)


def render_into(root: Path, api: Api) -> dict[str, str]:
    """Write every generated file of `api` under `root`; returns them."""
    files: dict[str, str] = cli.render(api)
    quiet(cli.write, root, files)
    return files


def tool(name: str) -> str | None:
    return shutil.which(name)


def run(argv: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(  # noqa: S603 -- resolved tool, argv from the test
        argv, cwd=cwd, capture_output=True, text=True, check=False, timeout=TOOL_TIMEOUT
    )


__all__ = ["Api", "parse"]
