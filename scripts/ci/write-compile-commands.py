#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Export a validated C/C++ compilation database from Ninja.

Meson 1.12 no longer materialises ``compile_commands.json`` during setup or
compile.  Ask Ninja for the two native compiler rules explicitly; an
unfiltered ``ninja -t compdb`` also emits custom, link and phony commands that
are not compilation-database entries.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any

COMPILER_RULES = ("c_COMPILER", "cpp_COMPILER")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".cu", ".hip", ".m", ".mm"}


def resolved_tool(name: str) -> str:
    tool = shutil.which(name)
    if tool is None:
        raise ValueError(f"required tool not found: {name}")
    return tool


async def invoke_ninja(
    ninja: str, build: Path, arguments: tuple[str, ...]
) -> tuple[int, bytes, bytes]:
    process = await asyncio.create_subprocess_exec(
        ninja,
        "-C",
        str(build),
        *arguments,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    stdout, stderr = await process.communicate()
    return process.returncode or 0, stdout, stderr


def run_ninja(ninja: str, build: Path, *arguments: str) -> str:
    returncode, stdout_bytes, stderr_bytes = asyncio.run(invoke_ninja(ninja, build, arguments))
    stdout = stdout_bytes.decode("utf-8")
    stderr = stderr_bytes.decode("utf-8")
    if returncode != 0:
        detail = stderr.strip() or stdout.strip() or "no diagnostic"
        raise ValueError(f"ninja {' '.join(arguments)} failed: {detail}")
    return stdout


def compiler_rules(ninja: str, build: Path) -> list[str]:
    available = {line.strip() for line in run_ninja(ninja, build, "-t", "rules").splitlines()}
    missing = [rule for rule in COMPILER_RULES if rule not in available]
    if missing:
        raise ValueError(f"Ninja manifest is missing compiler rules: {', '.join(missing)}")
    return list(COMPILER_RULES)


def validate_entry(entry: object, index: int) -> dict[str, Any]:
    if not isinstance(entry, dict):
        raise ValueError(f"compile entry {index} must be an object")
    directory = entry.get("directory")
    filename = entry.get("file")
    command = entry.get("command")
    arguments = entry.get("arguments")
    output = entry.get("output")
    if not isinstance(directory, str) or not directory or "\0" in directory:
        raise ValueError(f"compile entry {index} has no valid directory")
    if not Path(directory).is_absolute() or not Path(directory).is_dir():
        raise ValueError(f"compile entry {index} directory is not an existing absolute path")
    if not isinstance(filename, str) or not filename or "\0" in filename:
        raise ValueError(f"compile entry {index} has no valid file")
    if Path(filename).suffix.lower() not in SOURCE_SUFFIXES:
        raise ValueError(f"compile entry {index} is not a C/C++ source: {filename}")
    if command is not None and (not isinstance(command, str) or not command or "\0" in command):
        raise ValueError(f"compile entry {index} has invalid command")
    if arguments is not None and (
        not isinstance(arguments, list)
        or not arguments
        or not all(
            isinstance(argument, str) and argument and "\0" not in argument
            for argument in arguments
        )
    ):
        raise ValueError(f"compile entry {index} has invalid arguments")
    if command is None and arguments is None:
        raise ValueError(f"compile entry {index} has no valid command or arguments")
    if output is not None and (not isinstance(output, str) or not output or "\0" in output):
        raise ValueError(f"compile entry {index} has invalid output")
    return entry


def parse_database(raw: str) -> list[dict[str, Any]]:
    try:
        decoded = json.loads(raw)
    except json.JSONDecodeError as error:
        raise ValueError(f"Ninja emitted invalid JSON: {error}") from error
    if not isinstance(decoded, list):
        raise ValueError("Ninja compilation database must be an array")
    entries = [validate_entry(entry, index) for index, entry in enumerate(decoded)]
    if not entries:
        raise ValueError("Ninja compilation database is empty")
    return entries


def atomic_write(path: Path, entries: list[dict[str, Any]]) -> None:
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=".compile_commands.",
            suffix=".tmp",
            dir=path.parent,
            delete=False,
        ) as handle:
            temporary = Path(handle.name)
            json.dump(entries, handle, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        temporary.chmod(0o644)
        temporary.replace(path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--ninja", default="ninja")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        build = args.build_dir.resolve(strict=True)
        if not (build / "build.ninja").is_file():
            raise ValueError(f"Ninja manifest not found: {build / 'build.ninja'}")
        ninja = resolved_tool(args.ninja)
        rules = compiler_rules(ninja, build)
        entries = parse_database(run_ninja(ninja, build, "-t", "compdb", *rules))
        destination = build / "compile_commands.json"
        atomic_write(destination, entries)
    except (OSError, ValueError) as error:
        print(f"write-compile-commands: error: {error}", file=sys.stderr)
        return 1
    print(
        f"write-compile-commands: wrote {len(entries)} C/C++ commands to {destination}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
