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

Last, it refuses a workflow job that builds FFmpeg with `--disable-indevs` and then
runs an `ffmpeg-patches/test/*.sh` script that reads `-f lavfi` (itself or through
a helper it sources) unless the configure line adds `--enable-indev=lavfi`: that
build has no lavfi input device and the script fails to open its first input.

It also refuses a command that maps QSV frames straight to DRM PRIME
(`hwmap=derive_device=drm` with no `derive_device=vaapi` step before it on the
same line) within QSV_WINDOW lines after `-hwaccel qsv` or
`-hwaccel_output_format qsv`: FFmpeg maps a QSV frame only to its VAAPI parent
or to system memory, and the direct map fails with `Function not implemented`.
Markdown is read inside code fences only, so prose that explains the rule is
not a command.

Usage: run_ffmpeg_patch_contract_tests.py [--root DIR]; exit 1 names each
failing script with the tail of its output.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment

from scripts.lib.safe_subprocess import run as run_command

SERIES_DIR = "ffmpeg-patches"
TEST_GLOB = "test_*.py"
TAIL_LINES = 12
TIMEOUT_SECONDS = 120.0
PATCH_REF = re.compile(r"ffmpeg-patches/[0-9]{4}-[A-Za-z0-9._-]+\.patch")
LIVE_TREES = ("core", "dev", "docker", "tools", "mcp-server", "python", ".github", "scripts")
LIVE_FILES = (
    "AGENTS.md",
    "Containerfile.vmafx",
    "Dockerfile",
    "Makefile",
    "build-config.env",
    "ffmpeg-patches/README.md",
)
MAX_FILE_BYTES = 4_000_000
TEST_SCRIPT_RUN = re.compile(r"\bffmpeg-patches/test/[\w.-]+\.sh\b")
SOURCED_HELPER = re.compile(r"^\s*\.\s+\"\$\{HERE\}/([\w.-]+\.sh)\"", re.MULTILINE)
LAVFI_INPUT = "-f lavfi"
LAVFI_INDEV = "--enable-indev=lavfi"
QSV_ROOTS = ("docs", "scripts", "testdata", "ffmpeg-patches")
QSV_SUFFIXES = (".md", ".sh", ".py")
QSV_WINDOW = 12
QSV_DECODE = re.compile(r"-hwaccel(?:_output_format)?\s+qsv\b")
DRM_MAP = "hwmap=derive_device=drm"
VAAPI_MAP = "hwmap=derive_device=vaapi"
FENCE = re.compile(r"^\s*(```|~~~)")


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


def reads_lavfi(root: Path, script: str) -> bool:
    """True when the test script, or a helper it sources from its own folder, reads -f lavfi."""
    path = root / script
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8")
    helpers = [path.parent / name for name in SOURCED_HELPER.findall(text)]
    texts = [text] + [h.read_text(encoding="utf-8") for h in helpers if h.is_file()]
    return any(LAVFI_INPUT in body for body in texts)


def lavfi_without_indev(root: Path) -> list[str]:
    """'workflow:job: ...' for each job whose FFmpeg build lacks the lavfi device its tests read."""
    found = []
    for workflow in sorted((root / ".github" / "workflows").glob("*.yml")):
        jobs = (yaml.safe_load(workflow.read_text(encoding="utf-8")) or {}).get("jobs") or {}
        for job_id, job in jobs.items():
            runs = "\n".join(str(s.get("run", "")) for s in (job or {}).get("steps") or [])
            if "--disable-indevs" not in runs or LAVFI_INDEV in runs:
                continue
            for script in sorted(set(TEST_SCRIPT_RUN.findall(runs))):
                if reads_lavfi(root, script):
                    found.append(
                        f"{workflow.name}:{job_id}: runs {script}, which reads {LAVFI_INPUT}, "
                        f"on a --disable-indevs build without {LAVFI_INDEV}"
                    )
    return found


def _command_lines(path: Path) -> list[tuple[int, str]]:
    """(line number, text) of the lines that may be commands: in Markdown, inside fences."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if path.suffix != ".md":
        return list(enumerate(lines, 1))
    found, inside = [], False
    for number, line in enumerate(lines, 1):
        if FENCE.match(line):
            inside = not inside
        elif inside:
            found.append((number, line))
    return found


def _direct_drm_map(line: str) -> bool:
    """True when a DRM PRIME map on the line has no VAAPI map before it."""
    pos = line.find(DRM_MAP)
    return pos >= 0 and VAAPI_MAP not in line[:pos]


def qsv_direct_drm_maps(root: Path) -> list[str]:
    """'file:line: ...' for each DRM PRIME map of QSV frames that skips the VAAPI step."""
    found = []
    for tree in QSV_ROOTS:
        for path in sorted((root / tree).rglob("*")):
            if (
                path.suffix not in QSV_SUFFIXES
                or not path.is_file()
                or _is_fixture(path.relative_to(root))
            ):
                continue
            last_qsv = -QSV_WINDOW - 1
            for number, line in _command_lines(path):
                if QSV_DECODE.search(line):
                    last_qsv = number
                if number - last_qsv <= QSV_WINDOW and _direct_drm_map(line):
                    found.append(
                        f"{path.relative_to(root)}:{number}: maps QSV frames to DRM PRIME "
                        f"directly; map them with {VAAPI_MAP},format=vaapi first"
                    )
    return found


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
    problems = (
        stale_patch_refs(args.root, live_files(args.root))
        + lavfi_without_indev(args.root)
        + qsv_direct_drm_maps(args.root)
        + failures(args.root, scripts)
    )
    for line in problems:
        print(f"run_ffmpeg_patch_contract_tests: {line}", file=sys.stderr)
    if problems:
        return 1
    print(f"run_ffmpeg_patch_contract_tests: {len(scripts)} contract scripts pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
