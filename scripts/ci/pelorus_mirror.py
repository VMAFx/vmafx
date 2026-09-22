#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared exact-path policy for the read-only Pelorus mirror (ADR-1113)."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from collections.abc import Iterable
from pathlib import Path, PurePosixPath

MANIFEST_PATH = Path(__file__).with_name("pelorus-mirror-paths.txt")
NATIVE_SUFFIXES = frozenset(
    {
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".cu",
        ".cuh",
        ".h",
        ".hh",
        ".hip",
        ".hpp",
        ".hxx",
        ".inl",
        ".m",
        ".mm",
    }
)


def load_manifest(path: Path = MANIFEST_PATH) -> frozenset[str]:
    """Load and validate the exact lint-exempt path set."""
    entries = [
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if entries != sorted(entries):
        raise ValueError(f"Pelorus mirror manifest is not sorted: {path}")
    if len(entries) != len(set(entries)):
        raise ValueError(f"Pelorus mirror manifest contains duplicates: {path}")
    for entry in entries:
        parsed = PurePosixPath(entry)
        if parsed.is_absolute() or ".." in parsed.parts or str(parsed) != entry:
            raise ValueError(f"invalid Pelorus mirror path {entry!r} in {path}")
    return frozenset(entries)


EXACT_PELORUS_MIRROR_PATHS = load_manifest()


def normalise_repo_path(path: str) -> str:
    """Return a pre-commit/Git path in canonical repository-relative form."""
    while path.startswith("./"):
        path = path[2:]
    return PurePosixPath(path).as_posix()


def is_exact_pelorus_mirror(path: str) -> bool:
    """Return whether *path* is owned by the reviewed mirror manifest."""
    return normalise_repo_path(path) in EXACT_PELORUS_MIRROR_PATHS


def filter_paths(lines: Iterable[str]) -> list[str]:
    """Keep paths that remain under local lint/format ownership."""
    return [
        path for raw in lines if (path := raw.rstrip("\r\n")) and not is_exact_pelorus_mirror(path)
    ]


def run_clang_format(arguments: list[str]) -> int:
    """Run the pinned hook formatter after removing exact mirror paths."""
    options: list[str] = []
    paths: list[str] = []
    for argument in arguments:
        if argument.startswith("-"):
            options.append(argument)
        elif not is_exact_pelorus_mirror(argument):
            paths.append(argument)
    if not paths:
        return 0
    clang_format = shutil.which("clang-format")
    if clang_format is None:
        print("error: clang-format not found in the hook environment", file=sys.stderr)
        return 127
    return subprocess.run(  # noqa: S603 -- fixed executable supplied by hook env
        [clang_format, "-i", *options, *paths], check=False
    ).returncode


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "command",
        choices=("filter", "clang-format", "list", "suffixes"),
    )
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "filter":
        if args.arguments:
            build_parser().error("filter reads newline-delimited paths from stdin")
        for path in filter_paths(sys.stdin):
            print(path)
        return 0
    if args.command == "clang-format":
        return run_clang_format(args.arguments)
    if args.arguments:
        build_parser().error(f"{args.command} takes no arguments")
    values = (
        sorted(EXACT_PELORUS_MIRROR_PATHS) if args.command == "list" else sorted(NATIVE_SUFFIXES)
    )
    for value in values:
        print(value)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
