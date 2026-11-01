#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run every core/test contract script that reads the FFmpeg patch series.

The scripts under core/test/ that read `ffmpeg-patches/` (a patch file or a
`src/` filter source) run in the meson suite, which the local gate does not
build. A change to the series that moves or reformats what such a script
reads then fails only in CI. This runner finds those scripts by the
`ffmpeg-patches` path they name, so a new contract script is picked up with no
list to update, and runs each one with the interpreter running the runner.

It also refuses a live file that names a patch of the series that no longer
exists (`ffmpeg-patches/NNNN-*.patch`): code, build files, workflows and the
AGENTS.md files under the trees in LIVE_TREES. History (docs/, changelogs) and
test fixtures (scripts/**/test_*, scripts/**/tests/) may name retired patches.

Usage: run_ffmpeg_patch_contract_tests.py [--root DIR]; exit 1 names each
failing script with the tail of its output.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run as run_command

SERIES_DIR = "ffmpeg-patches"
TEST_GLOB = "test_*.py"
TAIL_LINES = 12
TIMEOUT_SECONDS = 120.0
PATCH_REF = re.compile(r"ffmpeg-patches/[0-9]{4}-[A-Za-z0-9._-]+\.patch")
LIVE_TREES = ("core", "dev", "docker", "tools", "mcp-server", "python", ".github", "scripts")
LIVE_FILES = ("AGENTS.md", "Makefile", "build-config.env", "ffmpeg-patches/README.md")
MAX_FILE_BYTES = 4_000_000


def contract_scripts(root: Path) -> list[Path]:
    """The core/test scripts whose text names the patch series directory."""
    return [
        path
        for path in sorted((root / "core" / "test").glob(TEST_GLOB))
        if SERIES_DIR in path.read_text(encoding="utf-8")
    ]


def _is_fixture(rel: Path) -> bool:
    """True for a test file under scripts/, whose patch names are made-up fixtures."""
    return rel.parts[0] == "scripts" and (
        "tests" in rel.parts or rel.name.startswith(("test_", "test-"))
    )


def live_files(root: Path) -> list[Path]:
    """The files in LIVE_TREES and LIVE_FILES that may not name a retired patch."""
    found = [root / name for name in LIVE_FILES if (root / name).is_file()]
    for tree in LIVE_TREES:
        for path in sorted((root / tree).rglob("*")):
            rel = path.relative_to(root)
            if path.is_file() and not _is_fixture(rel) and path.stat().st_size < MAX_FILE_BYTES:
                found.append(path)
    return found


def stale_patch_refs(root: Path, files: list[Path]) -> list[str]:
    """'file: ffmpeg-patches/NNNN-x.patch' for each named patch that does not exist."""
    stale = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for ref in sorted(set(PATCH_REF.findall(text))):
            if not (root / ref).is_file():
                stale.append(
                    f"{path.relative_to(root)}: names {ref}, which the series no longer has"
                )
    return stale


def failures(root: Path, scripts: list[Path]) -> list[str]:
    """One report per script that exits non-zero: its name and its output's tail."""
    found = []
    for script in scripts:
        result = run_command(
            [sys.executable, "-B", str(script)],
            allowed_executables=(sys.executable,),
            cwd=root,
            capture_output=True,
            stderr_to_stdout=True,
            text=True,
            timeout_seconds=TIMEOUT_SECONDS,
        )
        if result.returncode != 0:
            tail = "\n".join(result.stdout.splitlines()[-TAIL_LINES:])
            found.append(f"{script.relative_to(root)} (exit {result.returncode}):\n{tail}")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run the core/test contract scripts that read the FFmpeg patch series."
    )
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    scripts = contract_scripts(args.root)
    if not scripts:
        print(f"run_ffmpeg_patch_contract_tests: no core/test script names {SERIES_DIR}/")
        return 1
    problems = stale_patch_refs(args.root, live_files(args.root)) + failures(args.root, scripts)
    for line in problems:
        print(f"run_ffmpeg_patch_contract_tests: {line}", file=sys.stderr)
    if problems:
        return 1
    print(f"run_ffmpeg_patch_contract_tests: {len(scripts)} contract scripts pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
