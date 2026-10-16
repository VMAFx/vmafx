#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Add the MATLAB MEX and standalone C sources to compile_commands.json for the `cpu` lane.

This script adds compile-database entries for translation units that meson never builds:
1. The twelve MATLAB MEX sources under ``compat/python-vmaf/matlab/`` with the stub
   headers in ``scripts/ci/lint-stubs/matlab/`` (T-TIDY-MATLAB-MEX-UNMEASURED-2026-09-22).
2. The LD_PRELOAD fault injector ``ffmpeg-patches/test/fault_inject_libvmaf.c`` built
   against the installed/staged libvmaf headers.
3. The upstream parity harness ``scripts/dev/upstream_parity_harness.c`` built against
   internal headers and ``vmaf_feature_collector_get()``.

Usage: gen-mex-compile-commands.py <build-dir>

Existing entries for the same files are replaced. A tree missing any required source
or stub header exits 1, so the lane cannot go clean by measuring nothing.
"""

from __future__ import annotations

import json
import shlex
import sys
from collections.abc import Callable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MEX_ROOT = Path("compat/python-vmaf/matlab")
STUB_DIR = Path("scripts/ci/lint-stubs/matlab")
STUB_HEADERS = ("mex.h", "matrix.h")
FAULT_INJECT_SOURCE = Path("ffmpeg-patches/test/fault_inject_libvmaf.c")
HARNESS_SOURCE = Path("scripts/dev/upstream_parity_harness.c")
EXPECTED_ARGC = 2  # the script and the build directory


def mex_sources(repo_root: Path) -> list[Path]:
    """Every C source under the MATLAB harness, relative to *repo_root*."""
    return sorted(p.relative_to(repo_root) for p in (repo_root / MEX_ROOT).rglob("*.c"))


def mex_argv(source: Path, build_dir: Path, repo_root: Path) -> list[str]:
    return [
        "cc",
        "-std=gnu11",
        f"-I{repo_root / STUB_DIR}",
        f"-I{(repo_root / source).parent}",
        "-c",
        str(repo_root / source),
        "-o",
        "/dev/null",
    ]


def fault_inject_argv(source: Path, build_dir: Path, repo_root: Path) -> list[str]:
    return [
        "cc",
        "-std=gnu11",
        "-D_GNU_SOURCE",
        f"-I{build_dir / 'include'}",
        f"-I{repo_root / 'core/include'}",
        "-c",
        str(repo_root / source),
        "-o",
        "/dev/null",
    ]


def harness_argv(source: Path, build_dir: Path, repo_root: Path) -> list[str]:
    return [
        "cc",
        "-std=gnu11",
        "-O1",
        "-DPARITY_COLLECTOR_ACCESSOR",
        f"-I{repo_root / 'core/include'}",
        f"-I{build_dir / 'include'}",
        f"-I{repo_root / 'core/src'}",
        f"-I{build_dir / 'src'}",
        f"-I{build_dir}",
        "-c",
        str(repo_root / source),
        "-o",
        "/dev/null",
    ]


STANDALONE_BUILDERS: dict[Path, Callable[[Path, Path, Path], list[str]]] = {
    FAULT_INJECT_SOURCE: fault_inject_argv,
    HARNESS_SOURCE: harness_argv,
}


def entry_for(source: Path, build_dir: Path, repo_root: Path) -> dict[str, str]:
    """One compile-database entry for *source* (relative to *repo_root*)."""
    rel = source.relative_to(repo_root) if source.is_absolute() else source
    builder = STANDALONE_BUILDERS.get(rel, mex_argv)
    argv = builder(rel, build_dir, repo_root)
    return {
        "directory": str(build_dir),
        "command": shlex.join(argv),
        "file": str(repo_root / rel),
    }


def validate_sources(repo_root: Path) -> tuple[list[Path], list[str]]:
    """Return (sources, errors) checking all required files exist."""
    errors: list[str] = []
    missing_stubs = [h for h in STUB_HEADERS if not (repo_root / STUB_DIR / h).is_file()]
    if missing_stubs:
        errors.append(f"missing stub headers {missing_stubs}")
    sources = mex_sources(repo_root)
    if not sources:
        errors.append("no MEX source found")
    missing_standalone = [
        s for s in (FAULT_INJECT_SOURCE, HARNESS_SOURCE) if not (repo_root / s).is_file()
    ]
    if missing_standalone:
        errors.append(f"missing standalone sources {missing_standalone}")
    return [*sources, FAULT_INJECT_SOURCE, HARNESS_SOURCE], errors


def merge(existing: list[dict[str, str]], added: list[dict[str, str]]) -> list[dict[str, str]]:
    """*existing* without the entries *added* replaces, then *added*."""
    replaced = {entry["file"] for entry in added}
    return [entry for entry in existing if entry.get("file") not in replaced] + added


def main(argv: list[str]) -> int:
    if len(argv) != EXPECTED_ARGC:
        print(f"usage: {argv[0]} <build-dir>", file=sys.stderr)
        return 2
    build_dir = Path(argv[1]).resolve()
    sources, errors = validate_sources(REPO_ROOT)
    if errors:
        print(f"error: {'; '.join(errors)}", file=sys.stderr)
        return 1
    compdb = build_dir / "compile_commands.json"
    existing = json.loads(compdb.read_text(encoding="utf-8"))
    added = [entry_for(src, build_dir, REPO_ROOT) for src in sources]
    compdb.write_text(json.dumps(merge(existing, added), indent=2) + "\n", encoding="utf-8")
    print(f"gen-mex-compile-commands: {len(added)} extra entries in {compdb}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
