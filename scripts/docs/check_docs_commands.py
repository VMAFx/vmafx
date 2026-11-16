#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Hold docs/tutorials/ and docs/cookbook/ to their run-check commands (issue 2330).

Structural mode (default, wired into `make docs-fragments-check`):

- every `run-check` marker carries a unique id and a non-empty command;
- every tutorial page and every cookbook page except the exemptions below
  carries at least one run-check block, so a page cannot drift from the
  check while its commands drift from reality;
- every score-like number (three or more decimals) quoted on those pages is
  a recorded value — one of the pooled aggregates of the CPU golden
  snapshots or one of the RECORDED numbers with their sources. A freshly
  invented number fails, the same way a drifted flag name fails.

Execution mode (--execute): runs each block where its toolchain exists —
`cli` blocks need the binary (`--bin` or `$VMAF_BIN`), `ffmpeg` blocks need
FFmpeg with the encoder or filter the block names — and requires exit 0.
Rows whose toolchain is absent are skipped with a printed reason; a
requested execution that runs no block at all fails.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import sys
from pathlib import Path
from shutil import which
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

PAGES_DIRS = [ROOT / "docs" / "tutorials", ROOT / "docs" / "cookbook"]
SNAPSHOTS = sorted((ROOT / "testdata").glob("scores_cpu_*.json"))

EXIT_SUCCESS = 0
EXIT_FAILURE = 1
EXIT_UNREADABLE = 2

MARKER_RE = re.compile(r"<!--\s*run-check:([A-Za-z0-9_-]+)\s+exec=(cli|ffmpeg)\s*-->")
FENCE_OPEN_RE = re.compile(r"^```(?:bash|shell|sh)\s*$")
FENCE_CLOSE_RE = re.compile(r"^```\s*$")
NUMBER_RE = re.compile(r"\b\d+\.\d{3,}\b")

# Pages that deliberately carry no executable block (index pages, and
# hardware-gated recipes).
NO_BLOCK_PAGES = {"index.md", "two-gpus.md"}

# Numbers quoted from recorded sources (path -> what it is).
RECORDED = {
    "76.667830": "Research-0730 golden assertion (Netflix pair, v0.6.1)",
    "76.667831": "docs/models/overview.md v0.6.1 pooled mean (Netflix pair)",
    "83.782079": "Research-0730 reversed-pads wrong-direction score",
    "82.816060": "docs/models/overview.md default-model pooled mean",
    "82.816062": "docs/benchmarks.md 2026-09-06 default-model session value",
    "94.284562": "testdata/scores_cpu_576.json pooled harmonic mean",
    "94.32301": "testdata/scores_cpu_576.json pooled mean (the fixture-pair number)",
    "93.129024": "testdata/scores_cpu_576.json pooled min",
}


class Block(NamedTuple):
    block_id: str
    exec_class: str
    command: str
    page: str


def _read(path: Path) -> str:
    with path.open("rb") as handle:
        return handle.read().decode("utf-8")


def _page_rel(path: Path) -> str:
    """Repository-relative page name, or the bare name for test fixtures."""
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.name


def snapshot_numbers() -> set[str]:
    """Pooled aggregates of every CPU golden snapshot, as they are printed."""
    numbers: set[str] = set(RECORDED)
    for path in SNAPSHOTS:
        try:
            data = json.loads(_read(path))
        except (OSError, ValueError, KeyError):
            continue
        for metric in ("vmaf",):
            pooled = data.get("pooled_metrics", {}).get(metric, {})
            for value in pooled.values():
                numbers.add(f"{float(value):.6f}")
                numbers.add(f"{float(value):g}")
    return numbers


def parse_blocks(pages: list[tuple[str, str]]) -> list[Block]:
    """Collect run-check blocks across the tutorial and cookbook pages."""
    blocks: list[Block] = []
    for page_rel, text in pages:
        current: Block | None = None
        in_fence = False
        for line in text.split("\n"):
            if not in_fence:
                marker = MARKER_RE.search(line)
                if marker:
                    current = Block(marker.group(1), marker.group(2), "", page_rel)
                    continue
            if current is None:
                continue
            if FENCE_OPEN_RE.match(line):
                in_fence = True
                continue
            if in_fence:
                if FENCE_CLOSE_RE.match(line):
                    blocks.append(current)
                    current = None
                    in_fence = False
                elif line.strip():
                    current = current._replace(command=current.command + line + "\n")
    return blocks


def _join_command(block: Block) -> list[str]:
    """Split a documented block as a shell would: quotes group one argument,
    backslash-newline continues the line, and an unquoted `#` starts a comment."""
    try:
        return shlex.split(block.command.replace("\\\n", " "), comments=True, posix=True)
    except ValueError as exc:
        raise ValueError(f"cannot parse run-check block {block.block_id}: {exc}") from exc


def _number_scan_text(text: str) -> str:
    """Remove HTML comments and fenced blocks before scanning prose numbers."""
    text = re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL)
    return re.sub(r"```.*?```", "", text, flags=re.DOTALL)


def structural_failures(pages: list[tuple[str, str]], allowed_numbers: set[str]) -> list[str]:
    failures: list[str] = []
    blocks = parse_blocks(pages)
    seen: dict[str, str] = {}
    for block in blocks:
        if block.block_id in seen:
            failures.append(
                f"duplicate run-check id '{block.block_id}' on {block.page} and {seen[block.block_id]}"
            )
        seen[block.block_id] = block.page
        if not _join_command(block):
            failures.append(
                f"run-check block '{block.block_id}' on {block.page} has an empty command"
            )
    for page_rel, text in pages:
        if Path(page_rel).name not in NO_BLOCK_PAGES and "run-check:" not in text:
            failures.append(
                f"{page_rel}: no run-check block — every tutorial and cookbook page carries one"
            )
        for match in NUMBER_RE.finditer(_number_scan_text(text)):
            if match.group(0) not in allowed_numbers:
                failures.append(
                    f"{page_rel}: quotes {match.group(0)}, which no recorded source pins — "
                    "cite a snapshot aggregate or add the source to RECORDED"
                )
    return failures


def _resolve_cli_binary(args: argparse.Namespace) -> str | None:
    candidates: list[str] = []
    if args.bin:
        candidates.append(args.bin)
    if os.environ.get("VMAF_BIN"):
        candidates.append(os.environ["VMAF_BIN"])
    for candidate in candidates:
        if re.search(r"[\\/]", candidate):
            # safe_subprocess accepts a bare name or an absolute path, and CI
            # passes a workspace-relative build-docs-cli/tools/vmaf.
            return str(Path(candidate).resolve())
        found = which(candidate)
        if found:
            return str(Path(found).resolve())
    return None


def _safe_run(cmd: list[str], allowed: list[str]):
    """Run one argv and return the completed process (stdout/stderr intact)."""
    return run_command(
        cmd,
        allowed_executables=allowed,
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        timeout_seconds=900,
    )


def _ffmpeg_probe(argv: list[str]) -> tuple[bool, str]:
    """Return (toolchain_available, probe_output)."""
    needs_encoder = "-c:v" in argv
    needs_filter = next((a for a in argv if a.startswith("-lavfi")), None)
    filter_name = "vmafx"
    if needs_filter:
        match = re.search(r"(libvmaf|vmafx)=", needs_filter)
        filter_name = match.group(1) if match else "vmafx"
    if needs_encoder:
        probe = ["ffmpeg", "-hide_banner", "-h", "encoder=libx264"]
    elif needs_filter:
        probe = ["ffmpeg", "-hide_banner", "-h", "filter=" + filter_name]
    else:
        # A pure conversion block (no vmafx filter and no libx264) only needs
        # any ffmpeg, not the patched filter build.
        probe = ["ffmpeg", "-version"]
    try:
        result = _safe_run(probe, ["ffmpeg"])
    except Exception as exc:
        return False, str(exc)
    text = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        return False, text
    if needs_filter and "Unknown filter" in text:
        return False, text
    return True, text


def _prepare_block(
    block: Block, cli_binary: str | None
) -> tuple[list[str] | None, list[str], bool, str | None]:
    """Resolve the argv, allowed executables, score expectation and early failure."""
    argv = _join_command(block)
    if not argv:
        return None, [], False, f"run-check block '{block.block_id}' has an empty command"
    if block.exec_class == "cli":
        if cli_binary is None:
            print(f"SKIP {block.block_id} (no vmaf binary (--bin or $VMAF_BIN))")
            return None, [], False, None
        argv[0] = cli_binary
        return argv, [cli_binary], False, None
    if argv[0] != "ffmpeg":
        argv = ["ffmpeg", *argv]
    available, probe_text = _ffmpeg_probe(argv)
    if not available:
        what = "encoder libx264" if "-c:v" in argv else "filter vmafx"
        print(f"SKIP {block.block_id} (no ffmpeg carrying the {what})")
        if probe_text:
            print(f"  probe: {probe_text}")
        return None, [], False, None
    expect_score = any("vmafx=" in arg for arg in argv) and "-c:v" not in argv
    return argv, ["ffmpeg"], expect_score, None


def _run_block(block: Block, cli_binary: str | None) -> tuple[bool, str | None]:
    """Run one block. Returns (executed, failure). Skipped blocks are not executed."""
    argv, allowed, expect_score, failure = _prepare_block(block, cli_binary)
    if failure:
        return False, failure
    if argv is None:
        return False, None
    try:
        result = _safe_run(argv, allowed)
    except Exception as exc:
        return True, f"block {block.block_id} failed to run: {exc}"
    if result.returncode != 0:
        return True, f"block {block.block_id} exited {result.returncode}"
    if expect_score:
        output = (result.stdout or "") + (result.stderr or "")
        if not re.search(r"VMAF score: |vmafx \S+ mean: ", output):
            return True, f"block {block.block_id} printed no score line"
    print(f"OK {block.block_id}")
    return True, None


def _execute_blocks(
    args: argparse.Namespace, blocks: list[Block], cli_binary: str | None
) -> list[str]:
    failures: list[str] = []
    executed = 0
    for block in blocks:
        ran, failure = _run_block(block, cli_binary)
        executed += int(ran)
        if failure:
            failures.append(failure)
    if executed == 0:
        failures.append(
            "--execute ran no blocks (no toolchain present); a requested "
            "execution must execute at least the cli blocks"
        )
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bin", default=None, help="binary overriding $VMAF_BIN")
    parser.add_argument(
        "--execute",
        action="store_true",
        help="run the blocks where their toolchain exists (default: structural only)",
    )
    args = parser.parse_args(argv)

    pages: list[tuple[str, str]] = []
    try:
        for directory in PAGES_DIRS:
            for page in sorted(directory.glob("*.md")):
                pages.append((_page_rel(page), _read(page)))
        allowed_numbers = snapshot_numbers()
    except OSError as exc:
        print(f"UNREADABLE: {exc}")
        return EXIT_UNREADABLE

    failures = structural_failures(pages, allowed_numbers)
    if args.execute:
        blocks = parse_blocks(pages)
        failures.extend(_execute_blocks(args, blocks, _resolve_cli_binary(args)))
    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return EXIT_FAILURE
    print(
        f"OK — {len(parse_blocks(pages))} run-check blocks across "
        f"{len(pages)} pages, every quoted number recorded"
    )
    return EXIT_SUCCESS


if __name__ == "__main__":
    sys.exit(main())
