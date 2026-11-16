#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the migration-equivalence checker (issue 2331)."""

from __future__ import annotations

import json
import sys
import types
from collections.abc import Callable
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

import scripts.docs.check_migration_equivalence as checker
from scripts.docs.check_migration_equivalence import (
    EXIT_FAILURE,
    EXIT_SUCCESS,
    EXIT_UNREADABLE,
    check_page,
    main,
    parse_blocks,
    parse_rows,
    snapshot_pooled_mean,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
PAGE = REPO_ROOT / "docs" / "usage" / "migration.md"
SNAPSHOT = REPO_ROOT / "testdata" / "scores_cpu_576.json"

PAGE_TEXT = PAGE.read_text(encoding="utf-8")
POOLED = snapshot_pooled_mean(SNAPSHOT)

EXPECTED_IDS = {
    "cli-vmafx-default",
    "cli-netflix-compat",
    "python-run-vmaf",
    "ffmpeg-upstream",
    "ffmpeg-libvmaf",
    "fqm-vmaf",
    "easyvmaf-aligned",
    "easyvmaf-auto-prep",
}


def _fake_run(score: float) -> Callable[..., types.SimpleNamespace]:
    """A run_command stand-in: probes fail (so only cli rows execute), real
    invocations print one JSON pooled score."""

    def fake_run(
        cmd: list[str],
        allowed_executables: list[str],
        capture_output: bool,
        text: bool,
        timeout_seconds: float,
        check: bool = False,
    ) -> types.SimpleNamespace:
        if cmd[1:2] == ["-c"] or cmd[1:2] == ["-h"]:
            raise RuntimeError("probe refused")
        return types.SimpleNamespace(
            stdout=json.dumps({"pooled_metrics": {"vmaf": {"mean": score}}}),
            stderr="",
            returncode=0,
        )

    return fake_run


def test_real_page_passes_structurally() -> None:
    """The checked-in page holds against the checked-in snapshot."""
    assert check_page(PAGE_TEXT, POOLED) == []


def test_real_page_ids_and_blocks_pair_up() -> None:
    rows = parse_rows(PAGE_TEXT)
    blocks = parse_blocks(PAGE_TEXT)
    assert set(rows) == EXPECTED_IDS
    assert set(blocks) == EXPECTED_IDS - {"easyvmaf-auto-prep"}


def test_every_equivalent_row_quotes_the_snapshot_mean() -> None:
    for row in parse_rows(PAGE_TEXT).values():
        if not row.not_equivalent:
            assert row.expected == pytest.approx(POOLED, abs=1e-6)


def test_drifted_number_fails(tmp_path: Path) -> None:
    page = tmp_path / "migration.md"
    page.write_text(PAGE_TEXT.replace("`94.32301`", "`94.9`"), encoding="utf-8")
    assert main(["--page", str(page), "--snapshot", str(SNAPSHOT)]) == EXIT_FAILURE


def test_missing_adr_link_fails(tmp_path: Path) -> None:
    stripped = PAGE_TEXT.replace(
        "([Pairing the two inputs](ffmpeg.md#pairing-the-two-inputs), ADR-2165)",
        "([Pairing the two inputs](ffmpeg.md#pairing-the-two-inputs))",
    )
    page = tmp_path / "migration.md"
    page.write_text(stripped, encoding="utf-8")
    assert main(["--page", str(page), "--snapshot", str(SNAPSHOT)]) == EXIT_FAILURE


def test_equivalent_row_without_block_fails(tmp_path: Path) -> None:
    marker = "<!-- migration-check:cli-netflix-compat exec=cli -->\n"
    assert marker in PAGE_TEXT
    page = tmp_path / "migration.md"
    page.write_text(PAGE_TEXT.replace(marker, ""), encoding="utf-8")
    assert main(["--page", str(page), "--snapshot", str(SNAPSHOT)]) == EXIT_FAILURE


def test_not_equivalent_row_with_block_fails(tmp_path: Path) -> None:
    block = parse_blocks(PAGE_TEXT)["easyvmaf-aligned"].command
    marker = "<!-- migration-check:easyvmaf-auto-prep exec=wrapper -->\n"
    page = tmp_path / "migration.md"
    page.write_text(PAGE_TEXT + f"\n{marker}```bash\n{block}```\n", encoding="utf-8")
    assert main(["--page", str(page), "--snapshot", str(SNAPSHOT)]) == EXIT_FAILURE


def test_command_block_comments_are_not_arguments() -> None:
    """A trailing `# stock upstream` note stays a note, not an ffmpeg argument."""
    block = checker.TaggedBlock(
        "synthetic",
        "ffmpeg",
        'ffmpeg -i a.yuv -lavfi "libvmaf=x" -  # stock upstream\n',
    )
    argv = checker._join_command(block)
    assert "libvmaf=x" in argv
    assert "#" not in argv
    assert "stock" not in argv
    continued = checker.TaggedBlock("continued", "cli", "vmaf --a 1 \\\n    --b 2\n")
    assert checker._join_command(continued) == ["vmaf", "--a", "1", "--b", "2"]


def test_duplicate_check_row_is_unreadable(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    lines = PAGE_TEXT.splitlines()
    index = next(i for i, line in enumerate(lines) if line.startswith("| `cli-vmafx-default` |"))
    duplicate = lines[index]
    page = tmp_path / "migration.md"
    page.write_text(
        "\n".join([*lines[: index + 1], duplicate, *lines[index + 1 :]]) + "\n",
        encoding="utf-8",
    )
    assert main(["--page", str(page), "--snapshot", str(SNAPSHOT)]) == EXIT_UNREADABLE
    assert "UNREADABLE: duplicate check id" in capsys.readouterr().out


def test_probe_status_and_unknown_filter_skip_rows(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def fake_run(
        cmd: list[str],
        allowed_executables: list[str],
        capture_output: bool,
        text: bool,
        timeout_seconds: float,
        check: bool = False,
    ) -> types.SimpleNamespace:
        if cmd[1:2] == ["-c"]:
            return types.SimpleNamespace(returncode=1, stdout="", stderr="no vmaf")
        if cmd[1:2] == ["-h"]:
            return types.SimpleNamespace(returncode=0, stdout="", stderr="Unknown filter 'vmafx'.")
        return types.SimpleNamespace(returncode=0, stdout="", stderr="")

    monkeypatch.setattr(checker, "run_command", fake_run)
    skip, output = checker._run_python_row(["python", "-m", "vmaf"], None)
    assert skip is not None and "not importable" in skip
    assert output == ""
    skip, output = checker._run_ffmpeg_row(["ffmpeg", "-lavfi", "[0:v][1:v]vmafx=x"], None)
    assert skip is not None and "no ffmpeg carrying the vmafx filter" in skip
    assert output == ""


def test_ordinary_tables_are_not_check_rows(tmp_path: Path) -> None:
    page = tmp_path / "migration.md"
    page.write_text(
        "| Check id | Expected pooled mean | Status | Notes |\n"
        "|---|---|---|---|\n"
        "| `only-row` | `94.32301` | equivalent | — |\n"
        "\n"
        "| Tool | Scores | Notes |\n"
        "|---|---|---|\n"
        "| `ffmpeg-quality-metrics` | libvmaf scores | drives FFmpeg |\n",
        encoding="utf-8",
    )
    rows = parse_rows(page.read_text(encoding="utf-8"))
    assert list(rows) == ["only-row"]


def test_snapshot_mean_is_the_pinned_value() -> None:
    assert SNAPSHOT.is_file()
    data = json.loads(SNAPSHOT.read_text(encoding="utf-8"))
    assert data["backend_used"] == "cpu"
    assert pytest.approx(94.32301, abs=1e-6) == POOLED


def test_ffmpeg_parser_reads_the_json_stdout() -> None:
    """Every FFmpeg row sets log_fmt=json and log_path=/dev/stdout."""
    payload = json.dumps({"pooled_metrics": {"vmaf": {"mean": POOLED}}})
    assert checker._score_parsers()["ffmpeg"](payload) == pytest.approx(POOLED)


def test_cli_row_executes_and_compares(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """--execute with a stubbed runner: a matching score passes, a drifted one fails."""
    argv = [
        "--page",
        str(PAGE),
        "--snapshot",
        str(SNAPSHOT),
        "--execute",
        "--bin",
        str(tmp_path / "vmafx-stub"),
    ]
    monkeypatch.setattr(checker, "run_command", _fake_run(POOLED))
    assert main(argv) == EXIT_SUCCESS
    out = capsys.readouterr().out
    assert "OK cli-vmafx-default" in out
    assert "OK cli-netflix-compat" in out
    assert "SKIP python-run-vmaf" in out
    assert "SKIP ffmpeg-libvmaf" in out
    assert "SKIP fqm-vmaf" in out

    monkeypatch.setattr(checker, "run_command", _fake_run(94.99))
    assert main(argv) == EXIT_FAILURE
    assert "scored 94.99" in capsys.readouterr().out


def test_execute_without_toolchain_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("VMAF_BIN", raising=False)
    argv = [
        "--page",
        str(PAGE),
        "--snapshot",
        str(SNAPSHOT),
        "--execute",
        "--bin",
        str(tmp_path / "absent-vmaf"),
    ]
    # The cli rows fail to launch (absent binary) — the gate fails loudly.
    assert main(argv) == EXIT_FAILURE
