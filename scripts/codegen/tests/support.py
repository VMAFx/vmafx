# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared helpers of the VMAFx API generator tests (ADR-1852)."""

from __future__ import annotations

import io
import os
import re
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
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts" / "codegen"))

from vmafx_api import cli  # noqa: E402 -- path set above
from vmafx_api.loader import parse  # noqa: E402
from vmafx_api.model import Api  # noqa: E402

from scripts.lib.scratch_program import remove_program  # noqa: E402,F401 -- path set above

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


def next_patch(doc: dict[str, Any]) -> str:
    """The definition's abi_version with the patch raised by one: the bump an
    addition within 0.x needs, whatever version the live definition is at."""
    major, minor, patch = (int(part) for part in doc["api"]["abi_version"].split("."))
    return f"{major}.{minor}.{patch + 1}"


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


# clang-format's output differs between major versions (macro and initialiser
# alignment), so the format test runs only the major the repository pins for
# its clang-format hook; any other major would compare against another style.
CLANG_FORMAT_PIN = re.compile(r"mirrors-clang-format\s*\n\s*rev:\s*v(\d+)\.")
CLANG_FORMAT_VERSION = re.compile(r"clang-format version (\d+)\.")


def clang_format_pin(config: Path = ROOT / ".pre-commit-config.yaml") -> int:
    """The major version of the clang-format hook in .pre-commit-config.yaml."""
    found = CLANG_FORMAT_PIN.search(config.read_text(encoding="utf-8"))
    if found is None:
        raise AssertionError(f"no mirrors-clang-format rev in {config}")
    return int(found.group(1))


def clang_format_major(executable: str) -> int | None:
    found = CLANG_FORMAT_VERSION.search(run([executable, "--version"]).stdout)
    return int(found.group(1)) if found else None


def pinned_clang_format() -> tuple[str | None, str]:
    """The clang-format of the pinned major, and why none was found.

    VMAFX_CLANG_FORMAT names one explicitly and must be that major; otherwise
    clang-format-<major> and clang-format on PATH are tried in that order.
    """
    major = clang_format_pin()
    explicit = os.environ.get("VMAFX_CLANG_FORMAT")
    if explicit:
        found = clang_format_major(explicit)
        if found != major:
            raise AssertionError(f"VMAFX_CLANG_FORMAT={explicit} is {found}, the pin is {major}")
        return explicit, ""
    seen = []
    for candidate in (tool(f"clang-format-{major}"), tool("clang-format")):
        if candidate is None:
            continue
        found = clang_format_major(candidate)
        if found == major:
            return candidate, ""
        seen.append(f"{candidate} is {found}")
    return None, f"clang-format {major} (the pre-commit pin) not found: {'; '.join(seen) or 'none'}"


def run(argv: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(  # noqa: S603 -- resolved tool, argv from the test
        argv, cwd=cwd, capture_output=True, text=True, check=False, timeout=TOOL_TIMEOUT
    )


__all__ = ["Api", "parse"]
