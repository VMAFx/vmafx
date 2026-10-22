#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify that requires-python floors declared in pyproject.toml resolve.

Resolves each package at its declared Python floor with all extras using
`uv pip compile`.

Exit 0 clean, 1 a finding, 2 an operational error (uv missing, unreadable file, timeout).
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_python_dependency_locks import find_default_uv

EXCLUDED_PYPROJECTS = frozenset(
    {
        "pyproject.toml",
    }
)
# pyproject.toml at repository root declares workspace-level tooling and lint configuration
# targeting Python >=3.14 (see ADR-1282). It is not an individual installable package with
# a floor to resolve against lower Python versions; out of scope per ADR-3335.

DEFAULT_TIMEOUT_SECONDS = 60.0


class OperationalError(Exception):
    """An operational failure (missing tool, timeout, I/O error)."""


class Finding:
    """A floor or specification defect in a package."""

    def __init__(self, path: str, message: str):
        self.path = path
        self.message = message

    def __str__(self) -> str:
        return f"{self.path}: {self.message}"


def parse_floor(requires_python: str | None) -> tuple[str | None, str | None]:
    """Extract floor 'X.Y' from requires-python.

    Returns (floor, error_message). If floor is found, error_message is None.
    If missing or no '>=', floor is None and error_message is populated.
    """
    if requires_python is None:
        return None, "missing requires-python in [project]"
    if ">=" not in requires_python:
        return None, f"requires-python spec {requires_python!r} does not declare a '>=' floor"
    match = re.search(r">=\s*(\d+\.\d+)", requires_python)
    if not match:
        return None, f"requires-python spec {requires_python!r} has no valid '>=X.Y' version"
    return match.group(1), None


def discover_pyprojects(root: Path) -> list[Path]:
    """Find all tracked pyproject.toml files with a [project] table, excluding root."""
    git = shutil.which("git")
    tracked: list[str] = []
    if git and (root / ".git").exists():
        try:
            res = subprocess.run(  # noqa: S603 -- fixed argv, resolved git
                [git, "-C", str(root), "ls-files", "*pyproject.toml"],
                capture_output=True,
                text=True,
                check=True,
                timeout=15.0,
            )
            tracked = [line.strip() for line in res.stdout.splitlines() if line.strip()]
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError):
            tracked = []

    if not tracked:
        tracked = [
            str(p.relative_to(root))
            for p in root.rglob("pyproject.toml")
            if not any(part.startswith(".") for part in p.parts)
        ]

    candidates: list[Path] = []
    for rel_str in sorted(tracked):
        rel_posix = Path(rel_str).as_posix()
        if rel_posix in EXCLUDED_PYPROJECTS:
            continue
        full_path = root / rel_posix
        if not full_path.is_file():
            continue
        try:
            data = tomllib.loads(full_path.read_text(encoding="utf-8"))
        except (OSError, tomllib.TOMLDecodeError) as exc:
            raise OperationalError(f"cannot read {rel_posix}: {exc}") from exc
        if "project" in data and isinstance(data["project"], dict):
            candidates.append(full_path)
    return candidates


def compile_package(
    pyproject_path: Path,
    python_version: str,
    uv_binary: str,
    timeout_seconds: float,
) -> tuple[int, str]:
    """Compile pyproject.toml in a disposable temp directory using uv pip compile.

    Returns (returncode, error_message).
    """
    with tempfile.TemporaryDirectory(prefix="pyfloors-") as tmpdir:
        tmp_pyproject = Path(tmpdir) / "pyproject.toml"
        shutil.copy2(pyproject_path, tmp_pyproject)
        out_txt = Path(tmpdir) / "out.txt"
        cmd = [
            uv_binary,
            "pip",
            "compile",
            "pyproject.toml",
            "--all-extras",
            "--python-version",
            python_version,
            "--quiet",
            "-o",
            str(out_txt),
        ]
        try:
            res = subprocess.run(  # noqa: S603 -- validated argv, isolated temp directory
                cmd,
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=timeout_seconds,
            )
        except FileNotFoundError as exc:
            raise OperationalError(f"uv binary not found: {uv_binary}") from exc
        except subprocess.TimeoutExpired as exc:
            raise OperationalError(
                f"command timed out after {timeout_seconds:g}s: {' '.join(cmd)}"
            ) from exc
        except OSError as exc:
            raise OperationalError(f"failed to run uv: {exc}") from exc

        if res.returncode != 0:
            return res.returncode, first_error_line(res)
        return 0, ""


def first_error_line(res: subprocess.CompletedProcess[str]) -> str:
    """Return uv's first `error:` line, else its first non-empty line."""
    lines = [
        line.strip()
        for line in ((res.stderr or "") + "\n" + (res.stdout or "")).splitlines()
        if line.strip()
    ]
    for line in lines:
        if line.startswith(("error:", "error[")):
            return line
    if lines:
        return lines[0]
    return f"uv exited {res.returncode}"


def check_package(
    pyproject_path: Path,
    rel_path: str,
    uv_binary: str,
    timeout_seconds: float,
    tight: bool = False,
) -> list[Finding]:
    findings: list[Finding] = []
    try:
        data = tomllib.loads(pyproject_path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise OperationalError(f"cannot read {rel_path}: {exc}") from exc

    project = data.get("project")
    if not isinstance(project, dict):
        return []

    requires_python = project.get("requires-python")
    floor, err = parse_floor(requires_python)
    if err:
        findings.append(Finding(rel_path, err))
        return findings

    assert floor is not None
    code, msg = compile_package(pyproject_path, floor, uv_binary, timeout_seconds)
    if code != 0:
        findings.append(Finding(rel_path, f"floor {floor} does not resolve: {msg}"))
        return findings

    if tight:
        parts = floor.split(".", 1)
        major, minor = int(parts[0]), int(parts[1])
        if minor > 0:
            lower_floor = f"{major}.{minor - 1}"
            code_lower, _ = compile_package(pyproject_path, lower_floor, uv_binary, timeout_seconds)
            if code_lower == 0:
                findings.append(
                    Finding(
                        rel_path,
                        f"floor {floor} is not lowest; resolves at {lower_floor}",
                    )
                )

    return findings


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Verify requires-python floors resolve with all extras."
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Repository root directory",
    )
    parser.add_argument(
        "--uv-binary",
        default=None,
        help="Path or name of uv executable (default: the lock writer's uv lookup)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="Per-package compile timeout in seconds",
    )
    parser.add_argument(
        "--tight",
        action="store_true",
        help="Also check if floor - 0.1 resolves (finding if floor is not lowest)",
    )
    parser.add_argument(
        "files",
        nargs="*",
        help="Optional explicit pyproject.toml files to check",
    )
    return parser


def check_targets(
    targets: Sequence[Path], root: Path, uv_bin: str, timeout: float, tight: bool
) -> list[Finding]:
    """Check every target; OperationalError propagates to the caller."""
    all_findings: list[Finding] = []
    for target in targets:
        try:
            rel_path = target.relative_to(root).as_posix()
        except ValueError:
            rel_path = str(target)
        all_findings.extend(check_package(target, rel_path, uv_bin, timeout, tight=tight))
    return all_findings


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    root = args.root.resolve()
    uv_bin = args.uv_binary or find_default_uv(root)
    if not shutil.which(uv_bin) and not Path(uv_bin).is_file():
        print(f"check_python_floors: error: uv binary not found: {uv_bin}", file=sys.stderr)
        return 2

    try:
        if args.files:
            targets = [Path(f).resolve() for f in args.files]
        else:
            targets = discover_pyprojects(root)
        all_findings = check_targets(targets, root, uv_bin, args.timeout, args.tight)
    except OperationalError as exc:
        print(f"check_python_floors: error: {exc}", file=sys.stderr)
        return 2

    if all_findings:
        for finding in all_findings:
            print(str(finding))
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
