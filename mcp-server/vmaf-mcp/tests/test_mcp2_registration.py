# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Focused coverage for the MCP 2.x constructor-registration adapter."""

from __future__ import annotations

from types import SimpleNamespace
from typing import Any, cast

import pytest
from mcp.types import CallToolRequestParams

from vmaf_mcp import server as srv


def _context() -> Any:
    return cast(Any, SimpleNamespace(session=SimpleNamespace()))


@pytest.mark.asyncio
async def test_mcp2_handlers_are_registered_on_constructor() -> None:
    list_entry = srv.server.get_request_handler("tools/list")
    call_entry = srv.server.get_request_handler("tools/call")

    assert list_entry is not None
    assert list_entry.handler is srv._mcp_list_tools
    assert call_entry is not None
    assert call_entry.handler is srv._mcp_call_tool


@pytest.mark.asyncio
async def test_mcp2_list_tools_adapter_returns_typed_result() -> None:
    result = await srv._mcp_list_tools(_context(), None)

    assert "vmaf_score" in {tool.name for tool in result.tools}
    assert "list_models" in {tool.name for tool in result.tools}


@pytest.mark.asyncio
async def test_mcp2_call_adapter_preserves_iserror_contract() -> None:
    result = await srv._mcp_call_tool(
        _context(),
        CallToolRequestParams(name="no_such_tool", arguments={}),
    )

    assert result.is_error is True
    assert "unknown tool" in result.content[0].text
    assert srv._REQUEST_SESSION.get() is None


@pytest.mark.asyncio
async def test_mcp2_call_adapter_maps_wire_progress_token(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    observed: dict[str, Any] = {}

    async def _fake_call_tool(
        name: str,
        arguments: dict[str, Any],
        *,
        progress_token: str | int | None = None,
    ) -> list[Any]:
        observed.update(
            name=name,
            arguments=arguments,
            progress_token=progress_token,
        )
        return []

    monkeypatch.setattr(srv, "_call_tool", _fake_call_tool)
    params = CallToolRequestParams.model_validate(
        {
            "name": "vmaf_score",
            "arguments": {"ref": "ref.yuv"},
            "_meta": {"progressToken": "wire-token"},
        }
    )

    result = await srv._mcp_call_tool(_context(), params)

    assert result.is_error is False
    assert observed == {
        "name": "vmaf_score",
        "arguments": {"ref": "ref.yuv"},
        "progress_token": "wire-token",
    }
    assert srv._REQUEST_SESSION.get() is None


def test_cli_help_documents_transport(
    capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr("sys.argv", ["vmaf-mcp", "--help"])

    with pytest.raises(SystemExit) as exc_info:
        srv.main()

    assert exc_info.value.code == 0
    assert "--transport {stdio,http}" in capsys.readouterr().out
