#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Hold docs/usage/migration.md to its golden-pair equivalence rows (issue 2331).

Structural mode (default, wired into `make docs-fragments-check`): every
equivalent row must carry a `migration-check` command block and quote the
snapshot's pooled mean; every not-equivalent row must carry a reason with an
ADR link and no command block. A wrong or drifted number fails with no
binary needed.

Execution mode (--execute): runs each row's command where its toolchain
exists. `cli` rows run whenever the binary is found (`--bin` or `$VMAF_BIN`);
`python` rows need the compatibility package importable; `ffmpeg` rows need
an FFmpeg carrying the filter the row names; `wrapper` rows need the tool on
PATH and the wrapped fixture pair under testdata/migration-fixture/. Rows
whose toolchain is absent are skipped with a printed reason; an executed row
whose score misses the snapshot fails, and a requested execution that runs
no row at all fails too.
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
from typing import Callable, NamedTuple

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

REPO_ROOT = ROOT
DEFAULT_PAGE = REPO_ROOT / "docs" / "usage" / "migration.md"
DEFAULT_SNAPSHOT = REPO_ROOT / "testdata" / "scores_cpu_576.json"
FIXTURE_DIR = REPO_ROOT / "testdata" / "migration-fixture"

EXIT_SUCCESS = 0
EXIT_FAILURE = 1
EXIT_UNREADABLE = 2

# Score agreement: the page quotes the snapshot's six-decimal pooled mean and
# executed rows must land inside the rounding of that quote.
SCORE_TOLERANCE = 1e-6

MARKER_RE = re.compile(
    r"<!--\s*migration-check:([A-Za-z0-9_-]+)\s+exec=(cli|python|ffmpeg|wrapper)\s*-->"
)
FENCE_OPEN_RE = re.compile(r"^```(?:bash|shell|sh)\s*$")
FENCE_CLOSE_RE = re.compile(r"^```\s*$")
ROW_ID_RE = re.compile(r"^`([a-z0-9][a-z0-9-]*)`$")
FLOAT_RE = re.compile(r"^\d+\.\d+$")
ADR_RE = re.compile(r"ADR-\d{4}")
FILTER_RE = re.compile(r"\b(libvmaf|vmafx)=")
CHECK_TABLE_HEADER = "| Check id |"
MIN_CHECK_ROW_CELLS = 3


class Row(NamedTuple):
    row_id: str
    expected: float | None
    not_equivalent: bool


class TaggedBlock(NamedTuple):
    block_id: str
    exec_class: str
    command: str


def _read(path: Path) -> str:
    with path.open("rb") as handle:
        return handle.read().decode("utf-8")


def snapshot_pooled_mean(snapshot_path: Path) -> float:
    """Read the pooled vmaf mean the page must quote."""
    data = json.loads(_read(snapshot_path))
    pooled = data["pooled_metrics"]["vmaf"]["mean"]
    if not isinstance(pooled, (int, float)):
        raise ValueError(f"snapshot pooled mean is not a number: {pooled!r}")
    return float(pooled)


def parse_rows(text: str) -> dict[str, Row]:
    """Collect (id -> row) from the page's check tables.

    A table counts as a check table only when its header starts with
    ``| Check id |`` — ordinary tables in the same page are skipped.
    """
    rows: dict[str, Row] = {}
    in_check_table = False
    for line in text.split("\n"):
        if not line.startswith("|"):
            in_check_table = False
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if not in_check_table:
            in_check_table = line.startswith(CHECK_TABLE_HEADER)
            continue
        if len(cells) < MIN_CHECK_ROW_CELLS or set(cells[0]) <= {"-", ":"}:
            continue
        match = ROW_ID_RE.match(cells[0])
        if not match:
            continue
        row_id = match.group(1)
        if row_id in rows:
            raise ValueError(f"duplicate check id in page tables: {row_id}")
        expected_cell, status_cell = cells[1], cells[2]
        expected_match = FLOAT_RE.match(expected_cell.strip("`"))
        expected = float(expected_match.group(0)) if expected_match else None
        not_equivalent = "not equivalent" in status_cell.lower()
        if not_equivalent:
            if expected is not None:
                raise ValueError(f"not-equivalent row {row_id} must quote n/a, got {expected_cell}")
        elif expected is None:
            raise ValueError(
                f"equivalent row {row_id} must quote the pooled mean, got {expected_cell}"
            )
        rows[row_id] = Row(row_id, expected, not_equivalent)
    return rows


def parse_blocks(text: str) -> dict[str, TaggedBlock]:
    """Collect (id -> block) from the page's migration-check markers."""
    lines = text.split("\n")
    blocks: dict[str, TaggedBlock] = {}
    current: TaggedBlock | None = None
    in_fence = False
    for line in lines:
        if not in_fence:
            marker = MARKER_RE.search(line)
            if marker:
                if marker.group(1) in blocks:
                    raise ValueError(f"duplicate migration-check marker: {marker.group(1)}")
                current = TaggedBlock(marker.group(1), marker.group(2), "")
                continue
        if current is None:
            continue
        if in_fence:
            if FENCE_CLOSE_RE.match(line):
                blocks[current.block_id] = current
                current = None
                in_fence = False
            elif line.strip():
                current = current._replace(command=current.command + line + "\n")
            continue
        if FENCE_OPEN_RE.match(line):
            in_fence = True
            continue
    if current is not None:
        raise ValueError(f"unterminated migration-check block: {current.block_id}")
    return blocks


def _row_notes(text: str, row_id: str) -> str:
    for line in text.split("\n"):
        if line.startswith("|") and f"`{row_id}`" in line:
            return line
    return ""


def check_page(text: str, pooled_mean: float) -> list[str]:
    """Structural gate: return the list of failures (empty when the page holds)."""
    failures: list[str] = []
    rows = parse_rows(text)
    blocks = parse_blocks(text)
    if not rows:
        failures.append("page carries no check rows")
        return failures
    if not blocks:
        failures.append("page carries no migration-check command blocks")
        return failures
    for row_id, row in rows.items():
        block = blocks.get(row_id)
        if row.not_equivalent:
            if block is not None:
                failures.append(f"not-equivalent row {row_id} must not carry a command block")
            if not ADR_RE.search(_row_notes(text, row_id)):
                failures.append(f"not-equivalent row {row_id} must link an ADR in its notes")
        elif block is None:
            failures.append(f"equivalent row {row_id} has no migration-check block")
        elif row.expected is None or abs(row.expected - pooled_mean) > SCORE_TOLERANCE:
            failures.append(
                f"row {row_id} quotes {row.expected}, snapshot pooled mean is {pooled_mean}"
            )
    for block_id in blocks:
        if block_id not in rows:
            failures.append(f"migration-check block {block_id} has no table row")
    return failures


def _resolve_cli_binary(args: argparse.Namespace) -> str | None:
    candidates: list[str] = []
    if args.bin:
        candidates.append(args.bin)
    if os.environ.get("VMAF_BIN"):
        candidates.append(os.environ["VMAF_BIN"])
    for candidate in candidates:
        if re.search(r"[\\/]", candidate):
            return candidate
        found = which(candidate)
        if found:
            return found
    return None


def _safe_run(cmd: list[str], allowed: list[str]) -> str:
    result = run_command(
        cmd,
        allowed_executables=allowed,
        capture_output=True,
        text=True,
        timeout_seconds=900,
    )
    return (result.stdout or "") + (result.stderr or "")


def _json_pooled_mean(output: str) -> float:
    data = json.loads(output)
    if isinstance(data, dict) and "pooled_metrics" in data:
        return float(data["pooled_metrics"]["vmaf"]["mean"])
    if isinstance(data, dict) and "aggregate" in data:
        return float(data["aggregate"]["VMAF_score"])
    raise ValueError("unrecognized JSON score schema")


def _score_parsers() -> dict[str, Callable[[str], float]]:
    def pooled_from_json(output: str) -> float:
        return _json_pooled_mean(output)

    def ffmpeg_logged(output: str) -> float:
        match = re.search(r"(?:vmafx vmaf mean|VMAF score): ([0-9]+\.[0-9]+)", output)
        if not match:
            raise ValueError("no pooled score line in ffmpeg output")
        return float(match.group(1))

    def wrapper_json_or_frames(output: str) -> float:
        try:
            return _json_pooled_mean(output)
        except (ValueError, KeyError, json.JSONDecodeError):
            data = json.loads(output)
            scores = [
                float(value["vmaf"])
                for value in data.values()
                if isinstance(value, dict) and isinstance(value.get("vmaf"), (int, float))
            ]
            if not scores:
                raise ValueError("no per-frame vmaf scores in wrapper JSON") from None
            return sum(scores) / len(scores)

    return {
        "cli": pooled_from_json,
        "python": pooled_from_json,
        "ffmpeg": ffmpeg_logged,
        "wrapper": wrapper_json_or_frames,
    }


def _join_command(block: TaggedBlock) -> list[str]:
    """Split a documented command block into one argv, honouring backslash
    continuations, the shell's quoting and trailing `#` comments.

    The blocks are shell snippets, so shell rules apply: a backslash-newline
    continues the line, quotes group one argument, and an unquoted `#` starts
    a comment. Splitting on whitespace alone would keep the quotes (and any
    explanatory comment) as arguments of the command.
    """
    try:
        continued = block.command.replace("\\\n", " ")
        return shlex.split(continued, comments=True, posix=True)
    except ValueError as exc:
        raise ValueError(f"cannot parse command block {block.block_id}: {exc}") from exc


def _run_cli_row(argv: list[str], cli_binary: str | None) -> tuple[str | None, str]:
    if cli_binary is None:
        return "no vmaf binary (--bin or $VMAF_BIN)", ""
    argv[0] = cli_binary
    return None, _safe_run(argv, [cli_binary])


def _run_python_row(argv: list[str], cli_binary: str | None) -> tuple[str | None, str]:
    try:
        _safe_run([sys.executable, "-c", "import vmaf"], [sys.executable])
    except Exception:
        return "python package 'vmaf' not importable", ""
    argv[0] = sys.executable
    return None, _safe_run(argv, [sys.executable])


def _run_ffmpeg_row(argv: list[str], cli_binary: str | None) -> tuple[str | None, str]:
    filter_match = FILTER_RE.search(" ".join(argv))
    filter_name = filter_match.group(1) if filter_match else "libvmaf"
    try:
        _safe_run(["ffmpeg", "-h", f"filter={filter_name}"], ["ffmpeg"])
    except Exception:
        return f"no ffmpeg carrying the {filter_name} filter", ""
    return None, _safe_run(argv, ["ffmpeg"])


def _run_wrapper_row(argv: list[str], cli_binary: str | None) -> tuple[str | None, str]:
    tool = argv[0]
    if which(tool) is None:
        return f"tool not on PATH: {tool}", ""
    if not all((FIXTURE_DIR / name).is_file() for name in ("dis.mkv", "ref.mkv")):
        raise ValueError(
            f"tool present but wrapped fixture pair missing under "
            f"{FIXTURE_DIR.relative_to(REPO_ROOT)} — build it with the wrap commands "
            "in the migration page"
        )
    return None, _safe_run(argv, [tool])


ROW_RUNNERS = {
    "cli": _run_cli_row,
    "python": _run_python_row,
    "ffmpeg": _run_ffmpeg_row,
    "wrapper": _run_wrapper_row,
}


def _run_row(block: TaggedBlock, cli_binary: str | None) -> tuple[str | None, str]:
    """Run one row's command where its toolchain allows; return (skip reason, output).

    A non-None skip reason means the row did not run; otherwise the output
    string carries the row's score.
    """
    argv = _join_command(block)
    if not argv:
        raise ValueError("empty command")
    return ROW_RUNNERS[block.exec_class](argv, cli_binary)


def _execute_rows(
    args: argparse.Namespace, blocks: dict[str, TaggedBlock], pooled_mean: float
) -> list[str]:
    failures: list[str] = []
    parsers = _score_parsers()
    executed = 0
    cli_binary = _resolve_cli_binary(args)

    for block_id, block in blocks.items():
        try:
            skip, output = _run_row(block, cli_binary)
        except ValueError as exc:
            failures.append(f"row {block_id}: {exc}")
            continue
        except Exception as exc:
            failures.append(f"row {block_id} failed to run: {exc}")
            continue
        if skip is not None:
            print(f"SKIP {block_id} ({skip})")
            continue
        executed += 1
        try:
            score = parsers[block.exec_class](output)
        except (ValueError, KeyError, json.JSONDecodeError) as exc:
            failures.append(f"row {block_id} output unparseable: {exc}")
            continue
        if abs(score - pooled_mean) > SCORE_TOLERANCE:
            failures.append(f"row {block_id} scored {score}, snapshot pooled mean is {pooled_mean}")
        else:
            print(f"OK {block_id}: {score}")
    if executed == 0:
        failures.append(
            "--execute ran no rows (no toolchain present); a requested execution "
            "must execute at least the cli rows"
        )
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--page", type=Path, default=DEFAULT_PAGE)
    parser.add_argument("--snapshot", type=Path, default=DEFAULT_SNAPSHOT)
    parser.add_argument("--bin", default=None, help="binary overriding $VMAF_BIN")
    parser.add_argument(
        "--execute",
        action="store_true",
        help="run the rows where their toolchain exists (default: structural only)",
    )
    args = parser.parse_args(argv)

    try:
        text = _read(args.page)
        pooled_mean = snapshot_pooled_mean(args.snapshot)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"UNREADABLE: {exc}")
        return EXIT_UNREADABLE

    failures = check_page(text, pooled_mean)
    if args.execute:
        try:
            blocks = parse_blocks(text)
        except ValueError as exc:
            failures.append(str(exc))
        else:
            failures.extend(_execute_rows(args, blocks, pooled_mean))
    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return EXIT_FAILURE
    print("OK — migration equivalence rows match the snapshot")
    return EXIT_SUCCESS


if __name__ == "__main__":
    sys.exit(main())
