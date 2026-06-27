# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for the vmafx-mcp HTTP transport (ADR-0701, ADR-0967).

Tests use aiohttp's TestClient for lightweight in-process HTTP testing.
No network access, no GPU, no vmaf binary required for health/metrics endpoints.
The /v1/score endpoint is tested with a monkeypatched _run_vmaf_score coroutine.

Security tests (ADR-0967):
- Bearer auth enforcement (token set / wrong token / no token / NO_AUTH opt-out)
- Body size limit (Content-Length pre-flight → 413)
- Default bind host is 127.0.0.1
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any
from unittest.mock import AsyncMock, patch

import pytest
import pytest_asyncio

# Skip the entire module if aiohttp or prometheus_client are not installed.
aiohttp = pytest.importorskip("aiohttp")
pytest.importorskip("prometheus_client")

from aiohttp.test_utils import TestClient  # noqa: E402
from vmaf_mcp import http_transport as ht  # noqa: E402

# ---------------------------------------------------------------------------
# Helper: build a fresh app with an isolated prometheus registry
# ---------------------------------------------------------------------------


def _fresh_metrics() -> dict[str, Any]:
    """Return a metrics dict backed by an isolated prometheus registry.

    Using the default (global) registry in tests causes duplicate-metric errors
    when multiple tests call _build_metrics() in the same process.
    """
    import prometheus_client as pc  # type: ignore[import-untyped]

    registry = pc.CollectorRegistry(auto_describe=False)

    return {
        "scoring_requests_total": pc.Counter(
            "vmaf_scoring_requests_total_test",
            "Total VMAF scoring requests",
            ["endpoint", "status"],
            registry=registry,
        ),
        "scoring_errors_total": pc.Counter(
            "vmaf_scoring_errors_total_test",
            "Total VMAF scoring errors",
            registry=registry,
        ),
        "scoring_duration_seconds": pc.Histogram(
            "vmaf_scoring_duration_seconds_test",
            "VMAF scoring latency",
            buckets=[0.1, 0.5, 1.0, 5.0, 30.0, 120.0],
            registry=registry,
        ),
    }


# ---------------------------------------------------------------------------
# Fixtures
#
# All security fixtures are async generators (yield-based) so that the
# patch.dict context manager remains active for the entire test body.
# A non-generator fixture that does ``return await aiohttp_client(app)``
# inside a ``with patch.dict(...)`` block exits the context manager before
# the test body runs, silently restoring the env vars too early.
# ---------------------------------------------------------------------------


@pytest_asyncio.fixture
async def no_auth_client(aiohttp_client: Any) -> TestClient:
    """TestClient with VMAFX_MCP_HTTP_NO_AUTH=1 (auth disabled)."""
    with patch.dict(os.environ, {"VMAFX_MCP_HTTP_NO_AUTH": "1"}, clear=False):
        app = ht._make_app(_fresh_metrics())
        client = await aiohttp_client(app)
        yield client


@pytest_asyncio.fixture
async def token_client(aiohttp_client: Any) -> TestClient:
    """TestClient with VMAFX_MCP_HTTP_TOKEN=test-secret-token (auth enabled).

    Both env vars are managed inside the same patch.dict so that the
    removal of VMAFX_MCP_HTTP_NO_AUTH is properly reverted when the
    fixture tears down, preventing cross-test env leakage.
    """
    # Build a patched env dict: set the token and explicitly remove NO_AUTH.
    # patch.dict tracks *both* changes and reverts them on exit, so a
    # pre-existing VMAFX_MCP_HTTP_NO_AUTH in the test environment is not
    # permanently deleted if another fixture or test had set it.
    patch_env: dict[str, str] = {"VMAFX_MCP_HTTP_TOKEN": "test-secret-token"}
    saved_no_auth = os.environ.get("VMAFX_MCP_HTTP_NO_AUTH")
    with patch.dict(os.environ, patch_env, clear=False):
        # Remove NO_AUTH *inside* the patch.dict scope; patch.dict will
        # restore the original value (if any) when the context exits.
        os.environ.pop("VMAFX_MCP_HTTP_NO_AUTH", None)
        app = ht._make_app(_fresh_metrics())
        client = await aiohttp_client(app)
        try:
            yield client
        finally:
            # Explicitly restore NO_AUTH if it existed before this fixture
            # ran, guarding against the case where patch.dict exit order
            # races with the finally block.
            if saved_no_auth is not None:
                os.environ["VMAFX_MCP_HTTP_NO_AUTH"] = saved_no_auth
            else:
                os.environ.pop("VMAFX_MCP_HTTP_NO_AUTH", None)


@pytest_asyncio.fixture
async def no_token_client(aiohttp_client: Any) -> TestClient:
    """TestClient with no token and no NO_AUTH (locked-out mode)."""
    # Remove only the auth env vars; preserve everything else (PATH etc.).
    saved = {
        k: os.environ.pop(k)
        for k in ("VMAFX_MCP_HTTP_TOKEN", "VMAFX_MCP_HTTP_NO_AUTH")
        if k in os.environ
    }
    try:
        app = ht._make_app(_fresh_metrics())
        client = await aiohttp_client(app)
        yield client
    finally:
        os.environ.update(saved)


# Legacy fixture used by the original tests — runs in NO_AUTH mode so they
# pass without requiring a token.
@pytest_asyncio.fixture
async def test_client(aiohttp_client: Any) -> TestClient:
    """Return an aiohttp TestClient connected to a fresh app instance (NO_AUTH mode)."""
    with patch.dict(os.environ, {"VMAFX_MCP_HTTP_NO_AUTH": "1"}, clear=False):
        metrics = _fresh_metrics()
        app = ht._make_app(metrics)
        client = await aiohttp_client(app)
        yield client


# ---------------------------------------------------------------------------
# /healthz
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_healthz_returns_200(test_client: TestClient) -> None:
    """GET /healthz must return 200 with {"status": "healthy"}."""
    resp = await test_client.get("/healthz")
    assert resp.status == 200
    body = await resp.json()
    assert body == {"status": "healthy"}


@pytest.mark.asyncio
async def test_healthz_content_type(test_client: TestClient) -> None:
    """GET /healthz must return Content-Type: application/json."""
    resp = await test_client.get("/healthz")
    assert "application/json" in resp.content_type


# ---------------------------------------------------------------------------
# /readyz
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_readyz_returns_200_when_binary_exists(
    test_client: TestClient, tmp_path: Path
) -> None:
    """GET /readyz must return 200 when the vmaf binary path exists."""
    fake_binary = tmp_path / "vmaf"
    fake_binary.write_bytes(b"")
    fake_binary.chmod(0o755)

    with patch("vmaf_mcp.server._vmaf_binary", return_value=fake_binary):
        resp = await test_client.get("/readyz")
    assert resp.status == 200
    body = await resp.json()
    assert body["status"] == "ready"
    assert "vmaf_binary" in body


@pytest.mark.asyncio
async def test_readyz_returns_503_when_binary_missing(
    test_client: TestClient, tmp_path: Path
) -> None:
    """GET /readyz must return 503 when the vmaf binary path does not exist."""
    missing = tmp_path / "vmaf_does_not_exist"

    with patch("vmaf_mcp.server._vmaf_binary", return_value=missing):
        resp = await test_client.get("/readyz")
    assert resp.status == 503
    body = await resp.json()
    assert body["status"] == "not_ready"
    assert "reason" in body


# ---------------------------------------------------------------------------
# /metrics
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_metrics_returns_200(test_client: TestClient) -> None:
    """GET /metrics must return 200 with Prometheus exposition text."""
    resp = await test_client.get("/metrics")
    assert resp.status == 200


@pytest.mark.asyncio
async def test_metrics_content_type_is_prometheus(test_client: TestClient) -> None:
    """GET /metrics must use the prometheus_client CONTENT_TYPE_LATEST."""
    import prometheus_client as pc  # type: ignore[import-untyped]

    resp = await test_client.get("/metrics")
    # CONTENT_TYPE_LATEST may include version and charset info after a semicolon.
    assert resp.content_type.split(";")[0].strip() == pc.CONTENT_TYPE_LATEST.split(";")[0].strip()


@pytest.mark.asyncio
async def test_metrics_body_is_text(test_client: TestClient) -> None:
    """GET /metrics body must be valid UTF-8 Prometheus exposition text."""
    resp = await test_client.get("/metrics")
    text = await resp.text()
    # Prometheus exposition always has lines starting with # or a metric name.
    # An empty registry is valid; just check it round-trips as text.
    assert isinstance(text, str)


@pytest.mark.asyncio
async def test_metrics_full_content_type_header_preserved(test_client: TestClient) -> None:
    """The full ``CONTENT_TYPE_LATEST`` value — including ``charset=...`` —
    must reach the wire verbatim.

    Regression: aiohttp 3.13.5 added strict validation that rejects a
    ``charset=...`` fragment inside the ``content_type=`` kwarg of
    ``web.Response``, raising
    ``ValueError("charset must not be in content_type argument")`` at
    response construction.  ``prometheus_client`` ships
    ``CONTENT_TYPE_LATEST`` as the full RFC 1341 value
    (``text/plain; version=1.0.0; charset=utf-8``) and the Prometheus
    scrape contract expects this exact string — so the handler must
    write it via ``headers=`` rather than ``content_type=``.

    Pin the wire-level invariant: the raw ``Content-Type`` header on the
    response equals ``CONTENT_TYPE_LATEST`` byte-for-byte.
    """
    import prometheus_client as pc  # type: ignore[import-untyped]

    resp = await test_client.get("/metrics")
    assert resp.status == 200
    raw_header = resp.headers["Content-Type"]
    assert raw_header == pc.CONTENT_TYPE_LATEST, (
        f"Content-Type header drifted from CONTENT_TYPE_LATEST. "
        f"Expected {pc.CONTENT_TYPE_LATEST!r}, got {raw_header!r}. "
        f"If aiohttp now strips the charset fragment, the Prometheus "
        f"scrape contract is broken and the handler must be updated."
    )


# ---------------------------------------------------------------------------
# /v1/score
# ---------------------------------------------------------------------------


def _fake_score_payload() -> dict[str, Any]:
    return {
        "vmaf": 85.4321,
        "frames": [],
        "pooled_metrics": {"vmaf": {"mean": 85.4321, "harmonic_mean": 85.0}},
    }


@pytest.mark.asyncio
async def test_score_returns_200_on_valid_request(test_client: TestClient, tmp_path: Path) -> None:
    """POST /v1/score must return 200 with the vmaf JSON payload on success."""
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    fake_result = _fake_score_payload()

    async def _mock_run(req: Any) -> dict[str, Any]:
        return dict(fake_result)

    import vmaf_mcp.server as srv

    with (
        patch.object(srv, "_validate_path", side_effect=Path),
        patch.object(srv, "_run_vmaf_score", new=AsyncMock(side_effect=_mock_run)),
    ):
        resp = await test_client.post(
            "/v1/score",
            json={
                "reference": str(ref),
                "distorted": str(dis),
                "width": 1920,
                "height": 1080,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )

    assert resp.status == 200
    body = await resp.json()
    assert "vmaf" in body
    assert "request_id" in body


@pytest.mark.asyncio
async def test_score_precision_defaults_to_legacy(test_client: TestClient, tmp_path: Path) -> None:
    """Omitted precision must default to "legacy" (%.6f, ADR-0119).

    Regression for T-BUGHUNT-MCP-2026-06-27: the HTTP path previously defaulted
    precision to "17", diverging from the ScoreRequest default and the stdio /
    subprocess paths. A client must get the same numeric format from either
    transport.
    """
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    captured: dict[str, Any] = {}

    async def _mock_run(req: Any) -> dict[str, Any]:
        captured["precision"] = req.precision
        return _fake_score_payload()

    import vmaf_mcp.server as srv

    with (
        patch.object(srv, "_validate_path", side_effect=Path),
        patch.object(srv, "_run_vmaf_score", new=AsyncMock(side_effect=_mock_run)),
    ):
        resp = await test_client.post(
            "/v1/score",
            json={
                "reference": str(ref),
                "distorted": str(dis),
                "width": 1920,
                "height": 1080,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )

    assert resp.status == 200
    assert captured["precision"] == "legacy"


@pytest.mark.asyncio
async def test_score_returns_400_on_missing_fields(test_client: TestClient) -> None:
    """POST /v1/score must return 400 when required fields are absent."""
    resp = await test_client.post("/v1/score", json={"reference": "/tmp/r.yuv"})
    assert resp.status == 400
    body = await resp.json()
    assert "error" in body


@pytest.mark.asyncio
async def test_score_returns_400_on_invalid_json(test_client: TestClient) -> None:
    """POST /v1/score must return 400 when the body is not valid JSON."""
    resp = await test_client.post(
        "/v1/score",
        data=b"not-json",
        headers={"Content-Type": "application/json"},
    )
    assert resp.status == 400
    body = await resp.json()
    assert "error" in body


# ---------------------------------------------------------------------------
# Security tests (ADR-0967)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_unauthenticated_request_rejected(token_client: TestClient) -> None:
    """Request without Authorization header must return 401 when token is configured."""
    resp = await token_client.get("/healthz")
    assert resp.status == 401
    body = await resp.json()
    assert "error" in body
    assert "Unauthorized" in body["error"]


@pytest.mark.asyncio
async def test_wrong_token_rejected(token_client: TestClient) -> None:
    """Request with wrong Bearer token must return 401."""
    resp = await token_client.get(
        "/healthz",
        headers={"Authorization": "Bearer wrong-token"},
    )
    assert resp.status == 401
    body = await resp.json()
    assert "Unauthorized" in body["error"]


@pytest.mark.asyncio
async def test_correct_token_accepted(token_client: TestClient) -> None:
    """Request with correct Bearer token must be accepted (200 for /healthz)."""
    resp = await token_client.get(
        "/healthz",
        headers={"Authorization": "Bearer test-secret-token"},
    )
    assert resp.status == 200
    body = await resp.json()
    assert body["status"] == "healthy"


@pytest.mark.asyncio
async def test_no_token_configured_rejects_all(no_token_client: TestClient) -> None:
    """When VMAFX_MCP_HTTP_TOKEN is unset and NO_AUTH is not set, all requests return 401."""
    resp = await no_token_client.get("/healthz")
    assert resp.status == 401
    body = await resp.json()
    assert "error" in body
    assert "VMAFX_MCP_HTTP_TOKEN" in body["error"]


@pytest.mark.asyncio
async def test_body_too_large_rejected_via_content_length(no_auth_client: TestClient) -> None:
    """POST with Content-Length > 4 MiB must be rejected with 413."""
    oversized = ht.MAX_REQUEST_BODY_BYTES + 1
    resp = await no_auth_client.post(
        "/v1/score",
        data=b"x",  # actual bytes don't matter — Content-Length header drives the check
        headers={
            "Content-Type": "application/json",
            "Content-Length": str(oversized),
        },
    )
    assert resp.status == 413
    body = await resp.json()
    assert "error" in body
    assert "too large" in body["error"].lower()


@pytest.mark.asyncio
async def test_body_at_limit_accepted(monkeypatch: Any) -> None:
    """Content-Length exactly at MAX_REQUEST_BODY_BYTES must not trigger the 413 pre-flight.

    Tests the middleware logic directly via a mock request so we avoid sending
    4 MiB of actual TCP data (which would stall the TestClient connection).
    """
    import aiohttp.web as web

    # Build the middleware under NO_AUTH so the auth gate is bypassed.
    monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")
    monkeypatch.delenv("VMAFX_MCP_HTTP_TOKEN", raising=False)

    sentinel = web.Response(status=200, text="ok")
    handler_called = []

    async def _fake_handler(request: Any) -> Any:
        handler_called.append(True)
        return sentinel

    middleware_fn = ht._make_security_middleware()

    class _FakeHeaders:
        def get(self, key: str, default: str = "") -> str:
            return default

    class _FakeRequest:
        content_length: int | None = ht.MAX_REQUEST_BODY_BYTES  # exactly at limit
        headers = _FakeHeaders()

    result = await middleware_fn(_FakeRequest(), _fake_handler)
    # The middleware must have passed through to the handler (not 413).
    assert result is sentinel
    assert handler_called == [True]


@pytest.mark.asyncio
async def test_explicit_no_auth_allows_anonymous(no_auth_client: TestClient) -> None:
    """When VMAFX_MCP_HTTP_NO_AUTH=1, requests without Authorization are accepted."""
    resp = await no_auth_client.get("/healthz")
    assert resp.status == 200
    body = await resp.json()
    assert body["status"] == "healthy"


def test_default_bind_is_loopback(monkeypatch: Any) -> None:
    """Without VMAFX_MCP_HTTP_BIND, _resolve_bind_host() returns 127.0.0.1."""
    monkeypatch.delenv("VMAFX_MCP_HTTP_BIND", raising=False)
    assert ht._resolve_bind_host() == "127.0.0.1"


def test_explicit_bind_overrides_default(monkeypatch: Any) -> None:
    """VMAFX_MCP_HTTP_BIND=0.0.0.0 must override the default loopback."""
    monkeypatch.setenv("VMAFX_MCP_HTTP_BIND", "0.0.0.0")
    assert ht._resolve_bind_host() == "0.0.0.0"


def test_resolve_auth_token_returns_none_when_unset(monkeypatch: Any) -> None:
    """_resolve_auth_token() returns None when VMAFX_MCP_HTTP_TOKEN is absent."""
    monkeypatch.delenv("VMAFX_MCP_HTTP_TOKEN", raising=False)
    assert ht._resolve_auth_token() is None


def test_resolve_auth_token_returns_value_when_set(monkeypatch: Any) -> None:
    """_resolve_auth_token() returns the token string when the env var is set."""
    monkeypatch.setenv("VMAFX_MCP_HTTP_TOKEN", "my-secret")
    assert ht._resolve_auth_token() == "my-secret"


def test_no_auth_mode_false_by_default(monkeypatch: Any) -> None:
    """_no_auth_mode() returns False when VMAFX_MCP_HTTP_NO_AUTH is absent."""
    monkeypatch.delenv("VMAFX_MCP_HTTP_NO_AUTH", raising=False)
    assert ht._no_auth_mode() is False


def test_no_auth_mode_true_when_set(monkeypatch: Any) -> None:
    """_no_auth_mode() returns True when VMAFX_MCP_HTTP_NO_AUTH=1."""
    monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")
    assert ht._no_auth_mode() is True
