# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for shared AI CLI helpers."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

from aiutils.cli_helpers import add_batch_manifest_arguments, collect_cli_argv, make_argument_parser


def test_collect_cli_argv_uses_explicit_vector() -> None:
    assert collect_cli_argv(["--manifest", Path("batch.json")]) == ["--manifest", "batch.json"]


def test_collect_cli_argv_uses_sys_argv(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "argv", ["tool.py", "--report-json", "report.json"])

    assert collect_cli_argv(None) == ["--report-json", "report.json"]


def test_batch_manifest_arguments_parse_shared_options(tmp_path: Path) -> None:
    parser = make_argument_parser(description="batch runner")
    add_batch_manifest_arguments(parser, allow_row_failures=True)

    args = parser.parse_args(
        [
            "--manifest",
            str(tmp_path / "batch.json"),
            "--base-dir",
            str(tmp_path),
            "--report-json",
            str(tmp_path / "report.json"),
            "--report-md",
            str(tmp_path / "report.md"),
            "--allow-row-failures",
            "--fail-fast",
        ]
    )

    assert args.manifest == tmp_path / "batch.json"
    assert args.base_dir == tmp_path
    assert args.report_json == tmp_path / "report.json"
    assert args.report_md == tmp_path / "report.md"
    assert args.allow_row_failures is True
    assert args.fail_fast is True
