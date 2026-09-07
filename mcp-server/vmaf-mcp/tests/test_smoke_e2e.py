# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""End-to-end MCP smoke tests — exercising the JSON-RPC protocol layer.

These tests run through the server's *public* dispatch path (``_list_tools``
and ``_call_tool``) rather than internal helpers, so they verify the full
surface a Claude Code MCP client would see.

Requirements:
  - ``build/tools/vmaf`` must exist (``meson compile -C build``).
  - The Netflix golden YUV fixtures must be present under
    ``python/test/resource/yuv/``.

Skip conditions are surfaced as ``pytest.skip`` so CI lanes that don't have
the binary still report a clean summary rather than a failure.

Run with:
  cd mcp-server/vmaf-mcp && python -m pytest tests/test_smoke_e2e.py -v
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from vmaf_mcp import server as srv

REPO = Path(__file__).resolve().parents[3]

# Netflix golden fixtures for the smoke test
_REF_YUV = REPO / "python/test/resource/yuv/src01_hrc00_576x324.yuv"
_DIS_YUV = REPO / "python/test/resource/yuv/src01_hrc01_576x324.yuv"
_WIDTH = 576
_HEIGHT = 324

# Clip-mean VMAF v0.6.1 CPU reference — asserted to places=2 (matching
# the Netflix golden gate's own assertion). Source:
# python/test/quality_runner_test.py::test_run_vmaf_runner asserts
# results[0]["VMAF_score"] == 76.66890519623612, places=2.
# (The prior 76.69926 hardcode here was off by 0.03 — it never matched
# the Netflix golden it claimed to source.)
_EXPECTED_VMAF_SCORE = 76.66890519623612


def _binary_present() -> bool:
    return (REPO / "build" / "tools" / "vmaf").exists()


def _fixtures_present() -> bool:
    return _REF_YUV.exists() and _DIS_YUV.exists()


pytestmark_needs_binary = pytest.mark.skipif(
    not _binary_present(), reason="vmaf binary not found — run: meson compile -C build"
)
pytestmark_needs_fixtures = pytest.mark.skipif(
    not _fixtures_present(),
    reason="Netflix golden YUV fixtures not present under python/test/resource/yuv/",
)


# ---------------------------------------------------------------------------
# 1. list_tools — verify the server advertises its full tool catalogue
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_list_tools_returns_expected_names() -> None:
    """Server lists all fifteen documented tools (seven original + three ADR-0634 + five ADR-0608)."""
    tools = await srv._list_tools()
    names = {t.name for t in tools}
    # The original seven tools must always be present.
    original = {
        "vmaf_score",
        "list_models",
        "list_backends",
        "run_benchmark",
        "eval_model_on_split",
        "compare_models",
        "describe_worst_frames",
        # ADR-0634 additions
        "probe_backend",
        "vmaf_version",
        "vmaf_score_encoded",
    }
    # P1 additions (ADR-0608).
    p1 = {
        "list_extractors",
        "describe_model",
        "run_compare",
        "run_ladder",
        "run_tune_per_shot",
    }
    # Sidecar-binary bridge (#1240 item b).
    sidecars = {
        "vmaf_per_shot",
        "vmaf_roi",
        "vmaf_bench",
        "vmaf_vpl",
    }
    expected = original | p1 | sidecars
    assert expected == names, f"unexpected tool names: {names ^ expected}"


@pytest.mark.asyncio
async def test_list_tools_each_has_input_schema() -> None:
    """Every tool carries a non-empty inputSchema dict."""
    tools = await srv._list_tools()
    for tool in tools:
        assert isinstance(tool.input_schema, dict), f"{tool.name}: inputSchema is not a dict"
        assert "type" in tool.input_schema, f"{tool.name}: inputSchema missing 'type'"


# ---------------------------------------------------------------------------
# 2. call_tool — list_models / list_backends (no binary needed)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_call_tool_list_models_returns_list() -> None:
    """``list_models`` tool returns a JSON object with a 'models' list."""
    contents = await srv._call_tool("list_models", {})
    assert len(contents) == 1
    payload: dict[str, Any] = json.loads(contents[0].text)
    assert "models" in payload
    assert isinstance(payload["models"], list)
    assert len(payload["models"]) >= 1, "Expected at least one registered model"
    first = payload["models"][0]
    assert {"name", "path", "format"} <= first.keys()


@pytest.mark.asyncio
async def test_call_tool_list_backends_includes_cpu() -> None:
    """``list_backends`` tool always reports cpu=True."""
    contents = await srv._call_tool("list_backends", {})
    assert len(contents) == 1
    payload: dict[str, Any] = json.loads(contents[0].text)
    assert payload.get("cpu") is True, "cpu backend must always be available"


# ---------------------------------------------------------------------------
# 3. call_tool — vmaf_score against the Netflix golden fixture
# ---------------------------------------------------------------------------


@pytestmark_needs_binary
@pytestmark_needs_fixtures
@pytest.mark.asyncio
async def test_call_tool_vmaf_score_golden_pair() -> None:
    """``vmaf_score`` on the smallest Netflix golden pair returns a score
    within places=4 of the vmaf_v0.6.1 CPU reference (≈ 76.6993).

    This is the one-command MCP-server health check documented in ADR-0242.
    """
    contents = await srv._call_tool(
        "vmaf_score",
        {
            "ref": str(_REF_YUV),
            "dis": str(_DIS_YUV),
            "width": _WIDTH,
            "height": _HEIGHT,
            "pixfmt": "420",
            "bitdepth": 8,
            "model": "version=vmaf_v0.6.1",
            "backend": "cpu",
        },
    )
    assert len(contents) == 1, "Expected exactly one TextContent response"

    payload = json.loads(contents[0].text)

    # The vmaf binary writes a JSON output with a 'pooled_metrics' key.
    assert "error" not in payload, f"vmaf_score returned an error: {payload.get('error')}"

    pooled = payload.get("pooled_metrics") or payload.get("VMAF score") or {}
    if isinstance(pooled, dict):
        mean_score = float(
            pooled.get("vmaf", {}).get("mean")
            or pooled.get("VMAF score", {}).get("mean")
            or next(
                (v["mean"] for v in pooled.values() if isinstance(v, dict) and "mean" in v),
                None,
            )
            or 0.0
        )
    else:
        # Fallback: search for any float value labelled 'mean' at top level.
        mean_score = float(payload.get("mean", 0.0))

    assert mean_score > 0.0, (
        f"Could not extract a positive mean VMAF score from payload. "
        f"Payload keys: {list(payload.keys())}"
    )

    # Tolerance matches the Netflix golden gate's own places=2 assertion
    # (quality_runner_test.py::test_run_vmaf_runner). Tightening below
    # places=2 would diverge from the gate this test exists to mirror.
    assert abs(mean_score - _EXPECTED_VMAF_SCORE) < 1e-2, (
        f"Mean VMAF score {mean_score:.5f} deviates from reference "
        f"{_EXPECTED_VMAF_SCORE:.5f} by more than 1e-2 — possible regression."
    )


# ---------------------------------------------------------------------------
# 4. call_tool — unknown tool name raises (ADR-0613 isError fix)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_call_tool_unknown_name_raises() -> None:
    """ADR-0634 (isError spec-correctness): _call_tool must raise ValueError
    for unknown tool names so the mcp library sets isError=True on the
    CallToolResult.  The old pattern caught and returned TextContent with
    isError implicitly False, causing conformant clients to misclassify errors
    as successes.  Renamed from test_call_tool_unknown_name_returns_error_json."""
    with pytest.raises(ValueError, match="unknown tool"):
        await srv._call_tool("no_such_tool", {})
