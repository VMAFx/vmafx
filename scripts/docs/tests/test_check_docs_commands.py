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


def test_ffmpeg_probe_separates_conversion_from_the_filter_build(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A plain ffmpeg conversion does not need the vmafx filter build."""

    calls: list[list[str]] = []

    def fake_run(cmd: list[str], allowed: list[str]) -> types.SimpleNamespace:
        calls.append(cmd)
        if cmd[-1] == "-version":
            return types.SimpleNamespace(returncode=0, stdout="ffmpeg version", stderr="")
        return types.SimpleNamespace(returncode=0, stdout="", stderr="Unknown filter 'vmafx'.")

    monkeypatch.setattr(checker, "_safe_run", fake_run)
    conversion = ["ffmpeg", "-i", "a.yuv", "-pix_fmt", "yuv420p10le", "-f", "rawvideo", "b.yuv"]
    assert checker._ffmpeg_probe(conversion)[0] is True
    assert calls == [["ffmpeg", "-version"]]

    filtered = [
        "ffmpeg",
        "-i",
        "a.yuv",
        "-lavfi",
        "vmafx=model=version=vmaf_v0.6.1",
        "-f",
        "null",
        "-",
    ]
    assert checker._ffmpeg_probe(filtered)[0] is False
    assert calls[-1][-1] == "filter=vmafx"


def test_execute_runs_cli_and_skips_ffmpeg(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """--execute with a stubbed runner: cli blocks run, ffmpeg blocks skip."""

    def fake_run(
        cmd: list[str],
        allowed_executables: list[str],
        capture_output: bool,
        text: bool,
        timeout_seconds: float,
    ) -> types.SimpleNamespace:
        if cmd[0] == "ffmpeg":
            return types.SimpleNamespace(
                returncode=1,
                stdout="",
                stderr="Unknown filter 'vmafx'.",
            )
        assert cmd[0].endswith("stub-vmafx")
        return types.SimpleNamespace(returncode=0, stdout="", stderr="")

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


def test_relative_cli_binary_is_made_absolute(tmp_path: Path) -> None:
    args = types.SimpleNamespace(bin=str(tmp_path / "build" / "tools" / "vmaf"))
    resolved = checker._resolve_cli_binary(args)
    assert resolved is not None
    assert Path(resolved).is_absolute()
    assert Path(resolved) == (tmp_path / "build" / "tools" / "vmaf").resolve()


def test_number_scan_ignores_html_comments_and_fences() -> None:
    text = (
        "# Page\n\n"
        "<!-- a stale 91.74213 in a comment -->\n\n"
        "```text\n91.74213 inside a fence\n```\n\n"
        "The recorded value is 94.32301.\n"
    )
    assert checker.NUMBER_RE.findall(checker._number_scan_text(text)) == ["94.32301"]


def test_number_scan_boundaries() -> None:
    text = "two decimals 91.74, three 91.742, four 91.74213."
    assert checker.NUMBER_RE.findall(checker._number_scan_text(text)) == [
        "91.742",
        "91.74213",
    ]


def test_no_block_index_still_scans_numbers(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(checker, "PAGES_DIRS", [tmp_path])
    monkeypatch.setattr(checker, "SNAPSHOTS", [])
    (tmp_path / "index.md").write_text(
        "# Index\n\nNo block here, but the mean is 91.74213.\n", encoding="utf-8"
    )
    assert main([]) == EXIT_FAILURE


def test_block_nonzero_exit_fails(monkeypatch: pytest.MonkeyPatch) -> None:
    block = checker.Block("cli", "cli", "vmaf -r a -d b\n", "fake.md")
    monkeypatch.setattr(
        checker,
        "_safe_run",
        lambda cmd, allowed: types.SimpleNamespace(returncode=3, stdout="", stderr=""),
    )
    ran, failure = checker._run_block(block, "vmaf")
    assert ran is True
    assert failure is not None
    assert "exited 3" in failure


def test_ffmpeg_score_block_needs_a_score_line(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    block = checker.Block(
        "score",
        "ffmpeg",
        "ffmpeg -i a -lavfi vmafx=model=version=vmaf_v0.6.1 -f null -\n",
        "fake.md",
    )
    monkeypatch.setattr(checker, "_ffmpeg_probe", lambda argv: (True, ""))
    monkeypatch.setattr(
        checker,
        "_safe_run",
        lambda cmd, allowed: types.SimpleNamespace(returncode=0, stdout="", stderr=""),
    )
    ran, failure = checker._run_block(block, None)
    assert ran is True
    assert failure is not None
    assert "printed no score line" in failure

    monkeypatch.setattr(
        checker,
        "_safe_run",
        lambda cmd, allowed: types.SimpleNamespace(
            returncode=0, stdout="VMAF score: 94.32301\n", stderr=""
        ),
    )
    ran, failure = checker._run_block(block, None)
    assert ran is True
    assert failure is None
