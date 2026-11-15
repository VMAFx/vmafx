#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the tutorial/cookbook run-check docs command checker (issue 2330)."""

from __future__ import annotations

import sys
import types
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

import scripts.docs.check_docs_commands as checker
from scripts.docs.check_docs_commands import (
    EXIT_FAILURE,
    EXIT_SUCCESS,
    main,
    parse_blocks,
    snapshot_numbers,
)

PAGE = checker.ROOT / "docs" / "tutorials" / "score-a-pair.md"


def test_real_pages_pass_structurally() -> None:
    assert main([]) == EXIT_SUCCESS


def test_every_run_check_block_is_unique_and_nonempty() -> None:
    pages = [
        (p.relative_to(checker.ROOT).as_posix(), checker._read(p))
        for d in checker.PAGES_DIRS
        for p in sorted(d.glob("*.md"))
    ]
    blocks = parse_blocks(pages)
    ids = [b.block_id for b in blocks]
    assert len(ids) == len(set(ids)), "duplicate run-check ids"
    assert all(b.command.strip() for b in blocks)


def test_snapshot_numbers_are_pinned() -> None:
    numbers = snapshot_numbers()
    for expected in ("94.32301", "94.284562", "93.129024"):
        assert expected in numbers, expected
    for recorded in ("76.667830", "83.782079"):
        assert recorded in numbers, recorded


def test_invented_number_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(checker, "PAGES_DIRS", [tmp_path])
    monkeypatch.setattr(checker, "SNAPSHOTS", [])
    page = tmp_path / "fake.md"
    page.write_text(
        "# Fake\n\n<!-- run-check:x exec=cli -->\n"
        "```bash\nvmaf -r a -d b\n```\n\n"
        "The pooled mean is 91.74213 on this pair.\n",
        encoding="utf-8",
    )
    assert main([]) == EXIT_FAILURE


def test_recorded_number_passes(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(checker, "PAGES_DIRS", [tmp_path])
    monkeypatch.setattr(checker, "SNAPSHOTS", [])
    page = tmp_path / "fake.md"
    page.write_text(
        "# Fake\n\n<!-- run-check:x exec=cli -->\n"
        "```bash\nvmaf -r a -d b\n```\n\n"
        "The snapshot mean is 94.32301 — recorded.\n",
        encoding="utf-8",
    )
    assert main([]) == EXIT_SUCCESS


def test_duplicate_id_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(checker, "PAGES_DIRS", [tmp_path])
    marker = "<!-- run-check:same exec=cli -->\n```bash\nvmaf -r a -d b\n```\n"
    (tmp_path / "one.md").write_text("# One\n\n" + marker, encoding="utf-8")
    (tmp_path / "two.md").write_text("# Two\n\n" + marker, encoding="utf-8")
    assert main([]) == EXIT_FAILURE


def test_page_without_block_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(checker, "PAGES_DIRS", [tmp_path])
    (tmp_path / "empty.md").write_text("# Empty\n\nnothing here\n", encoding="utf-8")
    assert main([]) == EXIT_FAILURE


def test_execute_runs_cli_and_skips_ffmpeg(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """--execute with a stubbed runner: cli blocks run, ffmpeg blocks skip."""
    fake = types.SimpleNamespace(returncode=0)

    def fake_run(cmd, allowed_executables, capture_output, text, timeout_seconds):
        assert cmd[0].endswith("stub-vmafx") or cmd[0] == "ffmpeg"
        return fake

    monkeypatch.setattr(checker, "run_command", fake_run)
    monkeypatch.setattr(
        checker, "PAGES_DIRS", [checker.PAGES_DIRS[0]]
    )  # tutorials only, fewer blocks
    argv = ["--execute", "--bin", str(tmp_path / "stub-vmafx")]
    code = main(argv)
    out = capsys.readouterr().out
    assert "OK t1-score" in out
    assert code == EXIT_SUCCESS


def test_execute_without_any_toolchain_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(checker, "run_command", None)
    # The cli binary is absent, and ffmpeg probes raise: no block can run.
    argv = ["--execute", "--bin", str(tmp_path / "absent")]
    # _safe_run is patched away via run_command being None only if referenced;
    # instead patch _safe_run to raise for probes.
    monkeypatch.setattr(
        checker, "_safe_run", lambda cmd, allowed: (_ for _ in ()).throw(RuntimeError("no"))
    )
    assert main(argv) == EXIT_FAILURE
