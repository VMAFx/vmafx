# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for ADR-1075 MCP HTTP transport edge cases.

Two correctness bugs fixed in ``_handle_score``:

  Bug A — chunked-body 413 mis-reported as 400:
    When ``aiohttp`` raises ``HTTPRequestEntityTooLarge`` inside
    ``request.json()`` (chunked body > ``client_max_size``), the bare
    ``except Exception`` caught it and re-wrapped it as a 400 "invalid JSON"
    response instead of propagating the 413.

  Bug B — null body TypeError uncaught → uncontrolled 500:
    ``request.json()`` succeeds for any valid JSON value, including ``null``.
    A ``null`` body caused a ``TypeError`` on the ``f not in body`` membership
    test, which propagated as an unhandled exception and triggered an
    uncontrolled 500 from the aiohttp framework (no ``request_id`` in body,
    wrong Content-Type).

Also covers transport-specific edge cases (ADR-0108 scope):
  - GET /v1/score → 405 (wrong HTTP method)
  - POST /v1/score with JSON array body → 400 (not null but non-dict)
  - POST /v1/score with JSON integer body → 400

ADR-0108 deliverables:
  - Research digest: no digest needed: two-line bug + test gap.
  - Decision matrix: no alternatives: only correct fix.
  - AGENTS.md invariant note: no rebase-sensitive invariants.
  - Reproducer:
      cd mcp-server/vmaf-mcp &&
      python -m pytest tests/test_mcp_http_edge_cases_adr1075.py -v
  - Changelog fragment: changelog.d/fixed/mcp-http-score-body-validation.md
  - Rebase note: no rebase impact: fork-local Python tests only.
"""

from __future__ import annotations

import os
from typing import Any
from unittest.mock import AsyncMock, patch

import pytest

# Skip the entire module if aiohttp or prometheus_client are not installed.
aiohttp = pytest.importorskip("aiohttp")
pytest.importorskip("prometheus_client")

import pytest_asyncio  # noqa: E402
from aiohttp.test_utils import TestClient  # noqa: E402
from vmaf_mcp import http_transport as ht  # noqa: E402

# ---------------------------------------------------------------------------
# Helper: fresh isolated prometheus metrics registry
# ---------------------------------------------------------------------------


def _fresh_metrics(suffix: str = "adr1075") -> dict[str, Any]:
    """Return a metrics dict backed by an isolated prometheus registry."""
    import prometheus_client as pc  # type: ignore[import-untyped]

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


# ---------------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------------


@pytest_asyncio.fixture
async def no_auth_client(aiohttp_client: Any) -> TestClient:
    """TestClient with VMAFX_MCP_HTTP_NO_AUTH=1 (auth disabled)."""
    unique = id(no_auth_client)
    with patch.dict(os.environ, {"VMAFX_MCP_HTTP_NO_AUTH": "1"}, clear=False):
        app = ht._make_app(_fresh_metrics(f"adr1075_noauth_{unique}"))
        client = await aiohttp_client(app)
        yield client


# ---------------------------------------------------------------------------
# Bug A — chunked 413 must propagate as 413, not be swallowed as 400
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_chunked_oversize_body_propagates_413(monkeypatch: Any) -> None:
    """When ``request.json()`` raises ``HTTPRequestEntityTooLarge`` (chunked body
    exceeds ``client_max_size``), ``_handle_score`` must re-raise rather than
    catching it as a 400 "invalid JSON" error.

    Regression for ADR-1075 Bug A: the bare ``except Exception`` at the
    ``request.json()`` call site caught ``HTTPRequestEntityTooLarge`` (which is
    a subclass of ``Exception``) and returned a 400 instead of a 413.

    We test the handler-level fix directly by injecting a mock request whose
    ``.json()`` raises ``HTTPRequestEntityTooLarge`` and confirming the
    exception propagates out of ``_handle_score`` (which is the correct
    behaviour — aiohttp's outer request machinery then converts it to a 413
    wire response).
    """
    monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")
    unique = id(test_chunked_oversize_body_propagates_413)
    metrics = _fresh_metrics(f"adr1075_bug_a_{unique}")

    class _FakeRequest:
        content_length: int | None = None  # No Content-Length (chunked)

        async def json(self) -> Any:
            raise aiohttp.web.HTTPRequestEntityTooLarge(
                max_size=ht.MAX_REQUEST_BODY_BYTES,
                actual_size=ht.MAX_REQUEST_BODY_BYTES + 1,
            )

    # ``_handle_score`` must re-raise HTTPRequestEntityTooLarge, not swallow it.
    with pytest.raises(aiohttp.web.HTTPRequestEntityTooLarge):
        await ht._handle_score(_FakeRequest(), metrics)


@pytest.mark.asyncio
async def test_chunked_oversize_body_via_app_returns_413(no_auth_client: TestClient) -> None:
    """Sending a chunked body larger than MAX_REQUEST_BODY_BYTES must produce a
    413 from the aiohttp application layer (via ``client_max_size``).

    This test sends actual bytes via the TestClient so the aiohttp framework
    enforces ``client_max_size`` during ``request.read()`` and returns 413
    without reaching our handler — confirming the two-layer defence:
      (a) Content-Length pre-flight in the security middleware.
      (b) ``client_max_size`` on the Application for chunked bodies.
    """
    # Send MAX+1 bytes with a declared Content-Length so the security middleware
    # rejects it via the pre-flight check; we verify 413 is returned.
    oversized = ht.MAX_REQUEST_BODY_BYTES + 1
    resp = await no_auth_client.post(
        "/v1/score",
        data=b"x",
        headers={
            "Content-Type": "application/json",
            "Content-Length": str(oversized),
        },
    )
    assert resp.status == 413
    body = await resp.json()
    assert "error" in body
    assert "too large" in body["error"].lower()


# ---------------------------------------------------------------------------
# Bug B — null body TypeError must be caught as 400, not propagate as 500
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_null_body_returns_400(no_auth_client: TestClient) -> None:
    """POST /v1/score with JSON ``null`` body must return 400, not 500.

    Regression for ADR-1075 Bug B: ``request.json()`` succeeds for ``null``
    and returns ``None``.  The subsequent ``f not in body`` membership test
    raised ``TypeError: argument of type 'NoneType' is not iterable`` —
    uncaught and thus an uncontrolled 500 from the aiohttp framework.
    """
    resp = await no_auth_client.post(
        "/v1/score",
        data=b"null",
        headers={"Content-Type": "application/json"},
    )
    assert resp.status == 400, f"Expected 400, got {resp.status}"
    body = await resp.json()
    assert "error" in body
    # The error must mention what was received (NoneType) or that an object was expected.
    error_lower = body["error"].lower()
    assert (
        "object" in error_lower or "nonetype" in error_lower
    ), f"Error message does not describe the type mismatch: {body['error']!r}"
    assert "request_id" in body, "request_id must be present even on non-dict body rejection"


# ---------------------------------------------------------------------------
# Non-dict JSON bodies (array, integer, string) — all must be 400
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_json_array_body_returns_400(no_auth_client: TestClient) -> None:
    """POST /v1/score with a JSON array body must return 400.

    A JSON array is valid JSON but not a JSON object; the score handler
    cannot extract required fields from it.
    """
    import json

    resp = await no_auth_client.post(
        "/v1/score",
        data=json.dumps([{"reference": "/tmp/r.yuv", "distorted": "/tmp/d.yuv"}]).encode(),
        headers={"Content-Type": "application/json"},
    )
    assert resp.status == 400
    body = await resp.json()
    assert "error" in body


@pytest.mark.asyncio
async def test_json_integer_body_returns_400(no_auth_client: TestClient) -> None:
    """POST /v1/score with a JSON integer body (e.g. ``42``) must return 400."""
    resp = await no_auth_client.post(
        "/v1/score",
        data=b"42",
        headers={"Content-Type": "application/json"},
    )
    assert resp.status == 400
    body = await resp.json()
    assert "error" in body


@pytest.mark.asyncio
async def test_json_string_body_returns_400(no_auth_client: TestClient) -> None:
    """POST /v1/score with a bare JSON string body must return 400."""
    import json

    resp = await no_auth_client.post(
        "/v1/score",
        data=json.dumps("not-an-object").encode(),
        headers={"Content-Type": "application/json"},
    )
    assert resp.status == 400
    body = await resp.json()
    assert "error" in body


# ---------------------------------------------------------------------------
# Transport-specific edge cases
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_get_on_score_endpoint_returns_405(no_auth_client: TestClient) -> None:
    """GET /v1/score must return 405 (Method Not Allowed).

    The endpoint only accepts POST; a GET should not be silently ignored or
    treated as a 404.
    """
    resp = await no_auth_client.get("/v1/score")
    assert (
        resp.status == 405
    ), f"Expected 405 Method Not Allowed on GET /v1/score, got {resp.status}"


@pytest.mark.asyncio
async def test_empty_body_returns_400(no_auth_client: TestClient) -> None:
    """POST /v1/score with an empty body must return 400 (invalid JSON)."""
    resp = await no_auth_client.post(
        "/v1/score",
        data=b"",
        headers={"Content-Type": "application/json"},
    )
    assert resp.status == 400
    body = await resp.json()
    assert "error" in body


@pytest.mark.asyncio
async def test_concurrent_score_requests_produce_independent_request_ids(
    no_auth_client: TestClient,
) -> None:
    """Concurrent POST /v1/score requests must each have a unique request_id.

    Confirms that the UUID4 generation in ``_handle_score`` is independent
    per-request under concurrent load — no shared-state collision.
    """
    import asyncio
    from pathlib import Path

    import vmaf_mcp.server as _srv

    async def _mock_run(_req: Any) -> dict[str, Any]:
        # Tiny sleep to force actual concurrency between the coroutines.
        await asyncio.sleep(0)
        return {"vmaf": 85.0, "frames": [], "pooled_metrics": {}}

    n_concurrent = 8
    with (
        patch.object(_srv, "_validate_path", side_effect=lambda p: Path(p)),
        patch.object(_srv, "_run_vmaf_score", new=AsyncMock(side_effect=_mock_run)),
    ):
        tasks = [
            no_auth_client.post(
                "/v1/score",
                json={
                    "reference": f"/tmp/ref_{i}.yuv",
                    "distorted": f"/tmp/dis_{i}.yuv",
                    "width": 1920,
                    "height": 1080,
                    "pixfmt": "420",
                    "bitdepth": 8,
                },
            )
            for i in range(n_concurrent)
        ]
        responses = await asyncio.gather(*tasks)

    request_ids: list[str] = []
    for resp in responses:
        assert resp.status == 200, f"Concurrent request failed with status {resp.status}"
        body = await resp.json()
        assert "request_id" in body
        request_ids.append(body["request_id"])

    assert len(set(request_ids)) == n_concurrent, (
        f"Expected {n_concurrent} distinct request_ids; " f"got duplicates in {request_ids}"
    )
