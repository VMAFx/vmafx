# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the mcp-server-hardening fix wave (5 findings).

Fixes covered:
1. _nan_to_none RecursionError on deeply nested JSON (depth cap).
2. _pick_worst_frames: non-numeric frameNum returns 400 / clean skip.
3. HTTP _handle_score: TypeError on non-integer width/height returns 400.
4. describe_worst_frames: server-side enforce schema maximum:32 on n.
5. _call_tool: missing required tool arguments KeyError -> ValueError / 400.

Reproducer:
    cd mcp-server/vmaf-mcp && python -m pytest tests/test_mcp_hardening_wave1.py -v
"""

from __future__ import annotations

import asyncio
import math
from pathlib import Path
from typing import Any
from unittest.mock import AsyncMock, patch

import pytest

pytest.importorskip("mcp")

from vmaf_mcp import server as srv

# ---------------------------------------------------------------------------
# Fix 1: _nan_to_none depth cap prevents RecursionError
# ---------------------------------------------------------------------------


def _make_deep_dict(depth: int) -> Any:
    """Build a singly-nested dict ``{k: {k: {k: ...}}}`` of the given depth."""
    node: Any = {"leaf": 42.0}
    for _ in range(depth):
        node = {"child": node}
    return node


def test_nan_to_none_shallow_dict_unchanged():
    """Basic sanity: shallow dict with NaN/Inf values is normalised."""
    data = {"a": float("nan"), "b": float("inf"), "c": 1.5, "d": "text"}
    result = srv._nan_to_none(data)
    assert result["a"] is None
    assert result["b"] is None
    assert result["c"] == 1.5
    assert result["d"] == "text"


def test_nan_to_none_nested_list_normalised():
    data = [float("nan"), [float("inf"), 2.0], {"x": float("-inf")}]
    result = srv._nan_to_none(data)
    assert result[0] is None
    assert result[1][0] is None
    assert result[1][1] == 2.0
    assert result[2]["x"] is None


def test_nan_to_none_deep_nesting_no_recursion_error():
    """Deeply nested (>100 levels) must NOT raise RecursionError."""
    deep = _make_deep_dict(200)
    # Should not raise even though default recursion limit is ~1000 and we are
    # above the _max_depth=100 guard.
    result = srv._nan_to_none(deep)
    # The root level is processed; subtrees past depth=100 are replaced with None.
    assert isinstance(result, dict)


def test_nan_to_none_at_exactly_max_depth_boundary():
    """Subtrees at depth == _max_depth should still be processed; deeper capped."""
    # depth=100 nested dicts of {"v": nan} — the very deepest leaf should be
    # truncated (None) but values above it survive.
    deep = _make_deep_dict(110)
    result = srv._nan_to_none(deep)
    # At minimum the root dict is a dict (not None).
    assert isinstance(result, dict)


def test_nan_to_none_scalar_float_nan():
    assert srv._nan_to_none(float("nan")) is None


def test_nan_to_none_scalar_float_inf():
    assert srv._nan_to_none(float("inf")) is None
    assert srv._nan_to_none(float("-inf")) is None


def test_nan_to_none_scalar_non_float_passthrough():
    assert srv._nan_to_none(42) == 42
    assert srv._nan_to_none("hello") == "hello"
    assert srv._nan_to_none(None) is None


# ---------------------------------------------------------------------------
# Fix 2: _pick_worst_frames skips non-numeric frameNum without crashing
# ---------------------------------------------------------------------------


def _make_score_json(frames: list[dict[str, Any]]) -> dict[str, Any]:
    return {"frames": frames}


def test_pick_worst_frames_normal_case():
    """Standard path: numeric frameNums, returns n worst sorted ascending."""
    frames = [{"frameNum": i, "metrics": {"vmaf": float(100 - i)}} for i in range(10)]
    worst = srv._pick_worst_frames(_make_score_json(frames), n=3)
    assert len(worst) == 3
    scores = [s for _, s in worst]
    assert scores == sorted(scores)


def test_pick_worst_frames_string_framenum_skipped():
    """Non-numeric frameNum (e.g. 'bad') must be skipped, not raise TypeError."""
    frames = [
        {"frameNum": "bad", "metrics": {"vmaf": 50.0}},
        {"frameNum": 0, "metrics": {"vmaf": 70.0}},
        {"frameNum": 1, "metrics": {"vmaf": 60.0}},
    ]
    worst = srv._pick_worst_frames(_make_score_json(frames), n=5)
    # Only the two numeric frames survive; "bad" is skipped.
    assert len(worst) == 2
    frame_nums = [idx for idx, _ in worst]
    assert "bad" not in frame_nums
    assert 0 in frame_nums and 1 in frame_nums


def test_pick_worst_frames_none_framenum_skipped():
    """frameNum=None must be skipped (the original guard still works)."""
    frames = [
        {"frameNum": None, "metrics": {"vmaf": 10.0}},
        {"frameNum": 2, "metrics": {"vmaf": 80.0}},
    ]
    worst = srv._pick_worst_frames(_make_score_json(frames), n=5)
    assert len(worst) == 1
    assert worst[0][0] == 2


def test_pick_worst_frames_dict_framenum_skipped():
    """frameNum as a dict (malformed JSON) must be skipped."""
    frames = [
        {"frameNum": {"nested": "bad"}, "metrics": {"vmaf": 10.0}},
        {"frameNum": 3, "metrics": {"vmaf": 55.0}},
    ]
    worst = srv._pick_worst_frames(_make_score_json(frames), n=5)
    assert len(worst) == 1
    assert worst[0][0] == 3


def test_pick_worst_frames_nan_score_skipped():
    """Frames with NaN VMAF score are already skipped by the existing guard."""
    frames = [
        {"frameNum": 0, "metrics": {"vmaf": float("nan")}},
        {"frameNum": 1, "metrics": {"vmaf": 42.0}},
    ]
    worst = srv._pick_worst_frames(_make_score_json(frames), n=5)
    assert len(worst) == 1
    assert worst[0][0] == 1


# ---------------------------------------------------------------------------
# Fix 3: HTTP _handle_score returns 400 on non-integer width/height
# ---------------------------------------------------------------------------

# Counter of calls to _fresh_http_metrics so each test gets a unique metric
# name and avoids the prometheus duplicate-registration error.
_HTTP_TEST_COUNTER: int = 0


def _fresh_http_metrics() -> Any:
    """Return a metrics dict backed by an isolated prometheus registry with
    unique metric names.  Prometheus uses a global default registry; two calls
    to ``_build_metrics`` with the same name would raise
    ``ValueError: Duplicated timeseries``.  We suffix with a per-call counter
    to keep tests independent."""
    global _HTTP_TEST_COUNTER  # noqa: PLW0603  # module-level test counter
    _HTTP_TEST_COUNTER += 1
    suffix = f"h{_HTTP_TEST_COUNTER}"
    import prometheus_client as pc

    registry = pc.CollectorRegistry(auto_describe=False)
    return {
        "scoring_requests_total": pc.Counter(
            f"vmaf_scoring_requests_total_{suffix}",
            "Test counter",
            ["endpoint", "status"],
            registry=registry,
        ),
        "scoring_errors_total": pc.Counter(
            f"vmaf_scoring_errors_total_{suffix}",
            "Test errors",
            registry=registry,
        ),
        "scoring_duration_seconds": pc.Histogram(
            f"vmaf_scoring_duration_seconds_{suffix}",
            "Test latency",
            buckets=[0.1, 1.0, 10.0],
            registry=registry,
        ),
    }


@pytest.fixture
def _no_auth_env(monkeypatch):
    """Set VMAFX_MCP_HTTP_NO_AUTH=1 so security middleware passes through."""
    monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")
    monkeypatch.delenv("VMAFX_MCP_HTTP_TOKEN", raising=False)


@pytest.mark.asyncio
async def test_handle_score_null_width_returns_400(_no_auth_env, monkeypatch, tmp_path):
    """width=null in the JSON body must trigger a 400, not a 500 TypeError."""
    pytest.importorskip("aiohttp")
    pytest.importorskip("prometheus_client")
    from aiohttp.test_utils import TestClient, TestServer
    from vmaf_mcp import http_transport as ht

    metrics = _fresh_http_metrics()
    app = ht._make_app(metrics)

    # Create a valid-looking file pair so _validate_path passes, but width is null.
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x80" * 16)
    dis.write_bytes(b"\x80" * 16)

    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    async with TestClient(TestServer(app)) as client:
        resp = await client.post(
            "/v1/score",
            json={
                "reference": str(ref),
                "distorted": str(dis),
                "width": None,
                "height": 1080,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )
        assert resp.status == 400, f"expected 400, got {resp.status}"
        body = await resp.json()
        assert "error" in body


@pytest.mark.asyncio
async def test_handle_score_string_height_returns_400(_no_auth_env, monkeypatch, tmp_path):
    """height='abc' (non-integer string) must return 400."""
    pytest.importorskip("aiohttp")
    pytest.importorskip("prometheus_client")
    from aiohttp.test_utils import TestClient, TestServer
    from vmaf_mcp import http_transport as ht

    metrics = _fresh_http_metrics()
    app = ht._make_app(metrics)

    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x80" * 16)
    dis.write_bytes(b"\x80" * 16)

    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    async with TestClient(TestServer(app)) as client:
        resp = await client.post(
            "/v1/score",
            json={
                "reference": str(ref),
                "distorted": str(dis),
                "width": 1920,
                "height": "not-a-number",
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )
        assert resp.status == 400, f"expected 400, got {resp.status}"


# ---------------------------------------------------------------------------
# Fix 4: describe_worst_frames enforces n <= 32 server-side
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_call_tool_describe_worst_frames_n_above_32_raises(monkeypatch, tmp_path):
    """n=33 must raise ValueError before any scoring is attempted."""
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x80" * 16)
    dis.write_bytes(b"\x80" * 16)

    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    with pytest.raises(ValueError, match=r"'n' must be between 1 and 32"):
        await srv._call_tool_dispatch(
            "describe_worst_frames",
            {
                "ref": str(ref),
                "dis": str(dis),
                "width": 4,
                "height": 4,
                "pixfmt": "420",
                "bitdepth": 8,
                "n": 33,
            },
            progress_token=None,
        )


@pytest.mark.asyncio
async def test_call_tool_describe_worst_frames_n_below_1_raises(monkeypatch, tmp_path):
    """n=0 must raise ValueError."""
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x80" * 16)
    dis.write_bytes(b"\x80" * 16)

    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    with pytest.raises(ValueError, match=r"'n' must be between 1 and 32"):
        await srv._call_tool_dispatch(
            "describe_worst_frames",
            {
                "ref": str(ref),
                "dis": str(dis),
                "width": 4,
                "height": 4,
                "pixfmt": "420",
                "bitdepth": 8,
                "n": 0,
            },
            progress_token=None,
        )


@pytest.mark.asyncio
async def test_call_tool_describe_worst_frames_n_at_max_32_accepted(monkeypatch, tmp_path):
    """n=32 (the schema maximum) must pass the validation guard."""
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x80" * 16)
    dis.write_bytes(b"\x80" * 16)

    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    # The subsequent _run_vmaf_score call will fail (no real vmaf binary), but
    # the validation guard must NOT raise a ValueError for n=32.
    with patch.object(
        srv,
        "_run_vmaf_score",
        new=AsyncMock(side_effect=RuntimeError("vmaf binary not found (test stub)")),
    ):
        with pytest.raises(RuntimeError, match="vmaf binary not found"):
            await srv._call_tool_dispatch(
                "describe_worst_frames",
                {
                    "ref": str(ref),
                    "dis": str(dis),
                    "width": 4,
                    "height": 4,
                    "pixfmt": "420",
                    "bitdepth": 8,
                    "n": 32,
                },
                progress_token=None,
            )


# ---------------------------------------------------------------------------
# Fix 5: _call_tool KeyError on missing required arguments -> ValueError
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_call_tool_missing_ref_raises_value_error(monkeypatch, tmp_path):
    """Missing 'ref' argument for vmaf_score must surface as ValueError, not KeyError."""
    dis = tmp_path / "dis.yuv"
    dis.write_bytes(b"\x80" * 16)
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    with pytest.raises(ValueError, match=r"missing required argument.*'ref'"):
        await srv._call_tool(
            "vmaf_score",
            {
                # 'ref' intentionally omitted
                "dis": str(dis),
                "width": 4,
                "height": 4,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )


@pytest.mark.asyncio
async def test_call_tool_missing_features_raises_value_error(monkeypatch, tmp_path):
    """Missing 'features' argument for eval_model_on_split surfaces as ValueError."""
    model = tmp_path / "model.onnx"
    model.write_bytes(b"\x00" * 8)
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    with pytest.raises(ValueError, match=r"missing required argument.*'features'"):
        await srv._call_tool(
            "eval_model_on_split",
            {
                "model": str(model),
                # 'features' intentionally omitted
            },
        )


@pytest.mark.asyncio
async def test_call_tool_missing_backend_raises_value_error():
    """Missing 'backend' for probe_backend surfaces as ValueError, not KeyError."""
    with pytest.raises(ValueError, match=r"missing required argument.*'backend'"):
        await srv._call_tool("probe_backend", {})


@pytest.mark.asyncio
async def test_call_tool_missing_name_raises_value_error():
    """Missing 'name' for describe_model surfaces as ValueError."""
    with pytest.raises(ValueError, match=r"missing required argument.*'name'"):
        await srv._call_tool("describe_model", {})


@pytest.mark.asyncio
async def test_call_tool_missing_src_for_run_compare_raises_value_error():
    """Missing 'src' for run_compare surfaces as ValueError."""
    with pytest.raises(ValueError, match=r"missing required argument.*'src'"):
        await srv._call_tool("run_compare", {})
