#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Lint the tracked native sources configured by Meson (ADR-1142).

Keep the build database unchanged. A private analyzer database retains every
compile variant and translates only GCC's numeric LTO-thread spelling.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, cast

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".cu", ".hip", ".m", ".mm"}
GCC_LTO_THREADS = re.compile(r"-flto=[1-9][0-9]*\Z")


def tracked_sources(root: Path) -> set[Path]:
    git = shutil.which("git")
    if git is None:
        raise ValueError("required tool not found: git")
    result = subprocess.run(  # noqa: S603 -- resolved git argv, no shell
        [git, "-C", str(root), "ls-files", "-z"],
        check=True,
        capture_output=True,
        env={key: value for key, value in os.environ.items() if not key.startswith("GIT_")},
    )
    return {
        (root / name.decode()).resolve()
        for name in result.stdout.split(b"\0")
        if name and Path(name.decode()).suffix in SOURCE_SUFFIXES
    }


def entry_paths(entry: object, index: int) -> tuple[Path, Path]:
    if not isinstance(entry, dict):
        raise ValueError(f"compile entry {index} must be an object")
    directory, filename = entry.get("directory"), entry.get("file")
    if not isinstance(directory, str) or not directory:
        raise ValueError(f"compile entry {index} has no valid directory")
    if not isinstance(filename, str) or not filename:
        raise ValueError(f"compile entry {index} has no valid file")
    cwd = Path(directory)
    if not cwd.is_absolute():
        raise ValueError(f"compile entry {index} directory must be absolute")
    return cwd, (cwd / filename).resolve()


def entry_arguments(entry: dict[str, Any], index: int) -> list[str]:
    if "output" in entry and (
        not isinstance(entry["output"], str) or not entry["output"] or "\0" in entry["output"]
    ):
        raise ValueError(f"compile entry {index} has invalid output")
    if "arguments" in entry:
        argv = entry["arguments"]
        if (
            not isinstance(argv, list)
            or not argv
            or not all(isinstance(arg, str) and "\0" not in arg for arg in argv)
        ):
            raise ValueError(f"compile entry {index} has invalid arguments")
    else:
        command = entry.get("command")
        if not isinstance(command, str) or not command or "\0" in command:
            raise ValueError(f"compile entry {index} has no valid command or arguments")
        argv = shlex.split(command)
    if not argv or not argv[0]:
        raise ValueError(f"compile entry {index} has an empty compiler command")
    return cast(list[str], argv)


def prepare_database(build: Path, root: Path, report: Path) -> tuple[Path, list[Path]]:
    """Validate native entries and retain all tracked source command variants."""
    native = build / "compile_commands.json"
    raw = native.read_bytes()
    entries = json.loads(raw)
    if not isinstance(entries, list):
        raise ValueError("compile_commands.json must contain an array")
    tracked = tracked_sources(root)
    selected = []
    sources: set[Path] = set()
    excluded: set[str] = set()
    adaptations = []
    for index, entry in enumerate(entries):
        cwd, source = entry_paths(entry, index)
        argv = entry_arguments(entry, index)
        if not cwd.is_dir():
            raise ValueError(f"compile entry {index} directory is missing: {cwd}")
        if source not in tracked:
            excluded.add(str(source))
            continue
        if not source.is_file():
            raise ValueError(f"configured tracked source is missing: {source}")
        adapted = ["-flto" if GCC_LTO_THREADS.fullmatch(arg) else arg for arg in argv]
        for before, after in zip(argv, adapted, strict=True):
            if before != after:
                adaptations.append(
                    {"entry": index, "source": str(source), "from": before, "to": after}
                )
        # Do not deduplicate by source: test builds can carry different defines,
        # include directories and language settings for the same translation unit.
        prepared = dict(entry)
        prepared.pop("command", None)
        prepared.update(directory=str(cwd), file=str(source), arguments=adapted)
        selected.append(prepared)
        sources.add(source)
    if not selected:
        raise ValueError("compile database contains no configured tracked native sources")
    database = report / "compile_commands.json"
    database.write_text(json.dumps(selected, indent=2) + "\n", encoding="utf-8")
    manifest = {
        "input_database": str(native),
        "input_sha256": hashlib.sha256(raw).hexdigest(),
        "source_root": str(root),
        "configured_sources": [str(path.relative_to(root)) for path in sorted(sources)],
        "configured_command_count": len(selected),
        "unconfigured_tracked_sources": [
            str(path.relative_to(root)) for path in sorted(tracked - sources)
        ],
        "excluded_untracked_or_non_native_sources": sorted(excluded),
        "lto_adaptations": adaptations,
    }
    (report / "scope.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(
        f"Configured lint: {len(sources)} tracked native sources, {len(selected)} compile commands; "
        f"{len(tracked - sources)} tracked native sources outside this build profile.",
        flush=True,
    )
    print(f"Private analyzer database and receipts: {report}", flush=True)
    print(
        f"GCC numeric LTO arguments adapted: {len(adaptations)}; build database unchanged.",
        flush=True,
    )
    return database, sorted(sources)


def run_analyzer(argv: list[str], root: Path, log: Path) -> int:
    with log.open("w", encoding="utf-8") as stream:
        stream.write(f"$ {shlex.join(argv)}\n")
        stream.flush()
        try:
            result = subprocess.run(  # noqa: S603 -- resolved analyzer argv, no shell
                argv, cwd=root, stdout=stream, stderr=subprocess.STDOUT, check=False
            )
        except OSError as exc:
            stream.write(f"Cannot execute analyzer: {exc}\n")
            return 127
    return result.returncode


def cppcheck_arguments(binary: str, root: Path, database: Path) -> list[str]:
    """Analyze beyond branch budgets without overriding configured target settings."""
    return [
        binary,
        "--enable=all",
        "--check-level=exhaustive",
        "--inline-suppr",
        "--library=posix",
        f"--suppressions-list={root / '.cppcheck-suppressions.txt'}",
        f"--project={database}",
        "--error-exitcode=1",
    ]


def run(args: argparse.Namespace) -> int:
    root, build = args.repo_root.resolve(), args.build_dir.resolve()
    for attribute in ("clang_tidy", "cppcheck"):
        binary = getattr(args, attribute)
        resolved = shutil.which(binary)
        if resolved is None:
            raise ValueError(f"required analyzer not found: {binary}")
        setattr(args, attribute, str(Path(resolved).absolute()))
    # Per-run output avoids concurrent lint processes overwriting each other's
    # database/logs. It is disposable build output, retained for diagnosis.
    report = Path(tempfile.mkdtemp(prefix="lint-configured-", dir=build))
    database, sources = prepare_database(build, root, report)
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = {}
        for index, source in enumerate(sources):
            log = report / f"clang-tidy-{index:04d}.log"
            argv = [
                args.clang_tidy,
                "-p",
                str(report),
                "--quiet",
                *args.clang_tidy_arg,
                str(source),
            ]
            pending[pool.submit(run_analyzer, argv, root, log)] = (source, log)
        for future in concurrent.futures.as_completed(pending):
            source, log = pending[future]
            code = future.result()
            print(f"clang-tidy [{code}]: {source.relative_to(root)}", flush=True)
            if code:
                failures.append(str(source))
                print(log.read_text(encoding="utf-8", errors="replace"), end="", flush=True)
    # A clang-tidy failure must not prevent the independent cppcheck report.
    cppcheck_log = report / "cppcheck.log"
    cppcheck_code = run_analyzer(
        cppcheck_arguments(args.cppcheck, root, database),
        root,
        cppcheck_log,
    )
    print(cppcheck_log.read_text(encoding="utf-8", errors="replace"), end="", flush=True)
    result = {"clang_tidy_failed_sources": failures, "cppcheck_exit": cppcheck_code}
    (report / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return int(bool(failures or cppcheck_code))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--clang-tidy", default="clang-tidy")
    parser.add_argument("--cppcheck", default="cppcheck")
    parser.add_argument("--clang-tidy-arg", action="append", default=[])
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        return run(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"configured lint: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
