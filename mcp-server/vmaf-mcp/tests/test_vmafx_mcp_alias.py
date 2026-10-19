# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The wheel's ``vmafx-mcp`` script must not shadow the Go server silently.

``vmafx-mcp`` is the Go binary of ``cmd/vmafx-mcp``. The wheel used to install a
console script of the same name that ran the Python server. It is kept for one
release as a deprecated alias that says so on stderr and hands over to the Go
binary when one is on ``PATH``; ``vmaf-mcp`` is the Python server's own name.
"""

from __future__ import annotations

import stat
import tomllib
from pathlib import Path

import pytest

from vmaf_mcp import console_alias, server

_PYPROJECT = Path(__file__).resolve().parents[1] / "pyproject.toml"


def _executable(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\n", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def test_console_scripts_name_the_python_server_vmaf_mcp_only() -> None:
    scripts = tomllib.loads(_PYPROJECT.read_text(encoding="utf-8"))["project"]["scripts"]
    assert scripts["vmaf-mcp"] == "vmaf_mcp.server:main"
    assert scripts["vmafx-mcp"] == "vmaf_mcp.console_alias:deprecated_vmafx_mcp_alias"


def test_other_vmafx_mcp_skips_the_own_script(tmp_path: Path) -> None:
    own = _executable(tmp_path / "venv" / "bin" / "vmafx-mcp")
    go = _executable(tmp_path / "usr" / "bin" / "vmafx-mcp")
    search = f"{own.parent}:{go.parent}"
    assert console_alias.other_vmafx_mcp(str(own), search) == str(go)


def test_other_vmafx_mcp_is_none_when_only_the_own_script_exists(tmp_path: Path) -> None:
    own = _executable(tmp_path / "venv" / "bin" / "vmafx-mcp")
    assert console_alias.other_vmafx_mcp(str(own), str(own.parent)) is None
    assert console_alias.other_vmafx_mcp(str(own), "") is None
    assert console_alias.other_vmafx_mcp(str(own), f":{tmp_path / 'missing'}") is None


def test_alias_warns_on_stderr_and_hands_over_to_the_go_binary(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    own = _executable(tmp_path / "venv" / "bin" / "vmafx-mcp")
    go = _executable(tmp_path / "usr" / "bin" / "vmafx-mcp")
    handed: list[tuple[str, list[str]]] = []
    monkeypatch.setattr(console_alias.sys, "argv", [str(own), "--flag"])
    monkeypatch.setenv("PATH", f"{own.parent}:{go.parent}")
    monkeypatch.setattr(console_alias.os, "execv", lambda path, argv: handed.append((path, argv)))
    monkeypatch.setattr(server, "main", lambda: pytest.fail("ran the Python server"))
    console_alias.deprecated_vmafx_mcp_alias()
    captured = capsys.readouterr()
    assert handed == [(str(go), [str(go), "--flag"])]
    assert "deprecated" in captured.err
    assert "vmaf-mcp" in captured.err
    assert captured.out == ""  # stdout carries JSON-RPC


def test_alias_runs_the_python_server_when_no_go_binary_is_on_path(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    own = _executable(tmp_path / "venv" / "bin" / "vmafx-mcp")
    ran: list[bool] = []
    monkeypatch.setattr(console_alias.sys, "argv", [str(own)])
    monkeypatch.setenv("PATH", str(own.parent))
    monkeypatch.setattr(
        console_alias.os, "execv", lambda *_: pytest.fail("exec without a Go binary")
    )
    monkeypatch.setattr(server, "main", lambda: ran.append(True))
    console_alias.deprecated_vmafx_mcp_alias()
    assert ran == [True]
    assert "deprecated" in capsys.readouterr().err
