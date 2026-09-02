# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Round-5 coverage uplift for vmaf_mcp.http_transport (ADR-0701, ADR-0967).

Closes the residual 19 pp gap after rounds 1-4 for http_transport.py.
Specifically targets the lines reported as uncovered in the 81% baseline:

  - Lines 236-244: ``_build_ssl_context`` TLS-enabled branch (cert+key set).
  - Lines 494-497: ``_handle_score`` ValueError/FileNotFoundError 400 path
    (path-validation failure after all fields validated).
  - Lines 597-637: ``_serve`` coroutine (startup/warning branches + cleanup).
  - Lines 650-667: ``run_http_server`` synchronous entry point (happy + cancel
    + keyboard-interrupt paths).
  - Line 733: ``make_score_handler`` factory return.

All tests are self-contained — no network access, no vmaf binary, no GPU.

ADR-0108 deliverables:
- Research digest: no digest needed: coverage gap-fill.
- Decision matrix: no alternatives: only-one-way fix.
- AGENTS.md invariant note: no rebase-sensitive invariants.
- Reproducer:
    cd mcp-server/vmaf-mcp && python -m pytest tests/test_http_transport_round5.py -v
- Changelog fragment: changelog.d/added/mcp-server-coverage-round5.md.
- Rebase note: no rebase impact (fork-local Python tests only).
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import ssl
import subprocess
from pathlib import Path
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


def _fresh_metrics(suffix: str = "r5") -> dict[str, Any]:
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
# Helper: generate a minimal self-signed TLS certificate + key
# ---------------------------------------------------------------------------


def _generate_self_signed_cert(tmp_path: Path) -> tuple[Path, Path]:
    """Write a minimal self-signed TLS cert + key to *tmp_path*.

    Uses ``openssl req -x509`` so the test stays dependency-free (openssl is
    available on any system with Python's ssl module).  Returns (cert_path,
    key_path).
    """
    cert = tmp_path / "server.crt"
    key = tmp_path / "server.key"
    result = subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-keyout",
            str(key),
            "-out",
            str(cert),
            "-days",
            "1",
            "-nodes",
            "-subj",
            "/CN=localhost",
        ],
        capture_output=True,
        timeout=30,
    )
    if result.returncode != 0:
        pytest.skip(f"openssl not available or failed: {result.stderr.decode()}")
    return cert, key


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest_asyncio.fixture
async def no_auth_client(aiohttp_client: Any) -> TestClient:
    """TestClient with VMAFX_MCP_HTTP_NO_AUTH=1 (auth disabled)."""
    unique = id(no_auth_client)
    with patch.dict(os.environ, {"VMAFX_MCP_HTTP_NO_AUTH": "1"}, clear=False):
        app = ht._make_app(_fresh_metrics(f"r5_noauth_{unique}"))
        client = await aiohttp_client(app)
        yield client


# ---------------------------------------------------------------------------
# _build_ssl_context: TLS-enabled branch (lines 236-244)
# ---------------------------------------------------------------------------


def test_build_ssl_context_returns_context_when_env_set(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When both TLS env vars point to valid PEM files, an SSLContext is returned."""
    cert, key = _generate_self_signed_cert(tmp_path)
    monkeypatch.setenv("VMAFX_MCP_HTTP_TLS_CERT", str(cert))
    monkeypatch.setenv("VMAFX_MCP_HTTP_TLS_KEY", str(key))

    ctx = ht._build_ssl_context()
    assert ctx is not None
    assert isinstance(ctx, ssl.SSLContext)


def test_build_ssl_context_returns_none_when_only_cert_set(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When only CERT is set (KEY absent), _build_ssl_context returns None."""
    cert, _ = _generate_self_signed_cert(tmp_path)
    monkeypatch.setenv("VMAFX_MCP_HTTP_TLS_CERT", str(cert))
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_KEY", raising=False)

    ctx = ht._build_ssl_context()
    assert ctx is None


def test_build_ssl_context_logs_info_when_tls_enabled(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
) -> None:
    """When TLS is enabled the 'TLS enabled' info message is logged."""
    cert, key = _generate_self_signed_cert(tmp_path)
    monkeypatch.setenv("VMAFX_MCP_HTTP_TLS_CERT", str(cert))
    monkeypatch.setenv("VMAFX_MCP_HTTP_TLS_KEY", str(key))

    import logging

    with caplog.at_level(logging.INFO, logger="vmafx.http"):
        ht._build_ssl_context()

    assert any(
        "TLS enabled" in r.message for r in caplog.records
    ), f"Expected 'TLS enabled' in log records; got: {[r.message for r in caplog.records]}"


# ---------------------------------------------------------------------------
# _handle_score: path-validation 400 branch (lines 494-497)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_score_400_on_validate_path_file_not_found(
    no_auth_client: TestClient, tmp_path: Path
) -> None:
    """POST /v1/score with a non-existent path must return 400 via FileNotFoundError.

    Covers lines 494-497: the metrics counter + Response inside the
    ``except (ValueError, FileNotFoundError)`` block of ``_handle_score``.
    """
    import vmaf_mcp.server as _srv

    # _validate_path raises FileNotFoundError when the path does not exist.
    def _raise_fnf(p: str) -> Path:
        raise FileNotFoundError(f"path not found: {p}")

    with patch.object(_srv, "_validate_path", side_effect=_raise_fnf):
        resp = await no_auth_client.post(
            "/v1/score",
            json={
                "reference": "/nonexistent/ref.yuv",
                "distorted": "/nonexistent/dis.yuv",
                "width": 1920,
                "height": 1080,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )

    assert resp.status == 400
    body = await resp.json()
    assert "error" in body
    # The HTTP handler returns a generic message to avoid leaking internal paths.
    assert body["error"] == "invalid request parameters"
    assert "request_id" in body


@pytest.mark.asyncio
async def test_score_400_on_validate_path_value_error(
    no_auth_client: TestClient, tmp_path: Path
) -> None:
    """POST /v1/score where _validate_path raises ValueError must also return 400.

    Exercises the same except branch (ValueError) as the FileNotFoundError case.
    """
    import vmaf_mcp.server as _srv

    def _raise_ve(p: str) -> Path:
        raise ValueError(f"path traversal detected: {p}")

    with patch.object(_srv, "_validate_path", side_effect=_raise_ve):
        resp = await no_auth_client.post(
            "/v1/score",
            json={
                "reference": "../../etc/passwd",
                "distorted": "../../etc/shadow",
                "width": 1920,
                "height": 1080,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )

    assert resp.status == 400
    body = await resp.json()
    # The HTTP handler returns a generic message to avoid leaking internal paths.
    assert body["error"] == "invalid request parameters"


# ---------------------------------------------------------------------------
# _serve: startup warning branches (lines 597-637)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_serve_logs_warning_when_token_unset_and_no_auth_not_set(
    monkeypatch: pytest.MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    """_serve must log a warning when no token is configured and NO_AUTH is not set.

    Covers lines 614-620 inside ``_serve``.
    """
    import logging

    monkeypatch.delenv("VMAFX_MCP_HTTP_TOKEN", raising=False)
    monkeypatch.delenv("VMAFX_MCP_HTTP_NO_AUTH", raising=False)
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_CERT", raising=False)
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_KEY", raising=False)
    monkeypatch.setenv("VMAFX_MCP_HTTP_BIND", "127.0.0.1")

    # Patch TCPSite.start to be a no-op so we don't actually bind a port.
    started: list[bool] = []

    class _FakeSite:
        async def start(self) -> None:
            started.append(True)

    unique = id(test_serve_logs_warning_when_token_unset_and_no_auth_not_set)
    metrics = _fresh_metrics(f"r5_serve_warn_{unique}")

    with (
        patch.object(aiohttp.web, "TCPSite", return_value=_FakeSite()),
        patch("asyncio.Event.wait", side_effect=asyncio.CancelledError),
        caplog.at_level(logging.WARNING, logger="vmafx.http"),
        contextlib.suppress(asyncio.CancelledError, Exception),
    ):
        await ht._serve(port=0, metrics=metrics)

    warning_msgs = [r.message for r in caplog.records if r.levelno >= logging.WARNING]
    assert any(
        "VMAFX_MCP_HTTP_TOKEN" in m for m in warning_msgs
    ), f"Expected token-unset warning; got: {warning_msgs}"


@pytest.mark.asyncio
async def test_serve_logs_warning_when_no_auth_mode_enabled(
    monkeypatch: pytest.MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    """_serve must log a warning when VMAFX_MCP_HTTP_NO_AUTH=1 (open access).

    Covers lines 621-626 inside ``_serve``.
    """
    import logging

    monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")
    monkeypatch.delenv("VMAFX_MCP_HTTP_TOKEN", raising=False)
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_CERT", raising=False)
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_KEY", raising=False)
    monkeypatch.setenv("VMAFX_MCP_HTTP_BIND", "127.0.0.1")

    class _FakeSite:
        async def start(self) -> None:
            pass

    unique = id(test_serve_logs_warning_when_no_auth_mode_enabled)
    metrics = _fresh_metrics(f"r5_serve_noauth_{unique}")

    with (
        patch.object(aiohttp.web, "TCPSite", return_value=_FakeSite()),
        patch("asyncio.Event.wait", side_effect=asyncio.CancelledError),
        caplog.at_level(logging.WARNING, logger="vmafx.http"),
        contextlib.suppress(asyncio.CancelledError, Exception),
    ):
        await ht._serve(port=0, metrics=metrics)

    warning_msgs = [r.message for r in caplog.records if r.levelno >= logging.WARNING]
    assert any(
        "NO_AUTH" in m or "authentication disabled" in m for m in warning_msgs
    ), f"Expected NO_AUTH warning; got: {warning_msgs}"


@pytest.mark.asyncio
async def test_serve_calls_runner_cleanup_on_exit(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """_serve must call ``runner.cleanup()`` in its finally block on cancellation.

    Covers lines 631-635 (the ``finally`` block of ``_serve``).
    """
    monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_CERT", raising=False)
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_KEY", raising=False)
    monkeypatch.setenv("VMAFX_MCP_HTTP_BIND", "127.0.0.1")

    cleanup_called: list[bool] = []

    class _FakeRunner:
        async def setup(self) -> None:
            pass

        async def cleanup(self) -> None:
            cleanup_called.append(True)

    class _FakeSite:
        async def start(self) -> None:
            pass

    unique = id(test_serve_calls_runner_cleanup_on_exit)
    metrics = _fresh_metrics(f"r5_cleanup_{unique}")

    with (
        patch.object(aiohttp.web, "AppRunner", return_value=_FakeRunner()),
        patch.object(aiohttp.web, "TCPSite", return_value=_FakeSite()),
        patch("asyncio.Event.wait", side_effect=asyncio.CancelledError),
        contextlib.suppress(asyncio.CancelledError, Exception),
    ):
        await ht._serve(port=0, metrics=metrics)

    assert cleanup_called, "runner.cleanup() was not called in the finally block"


# ---------------------------------------------------------------------------
# run_http_server: synchronous entry point (lines 650-667)
# ---------------------------------------------------------------------------


def _isolated_build_metrics(suffix: str) -> Any:
    """Return a ``_build_metrics`` replacement that uses an isolated registry.

    ``run_http_server`` calls ``_build_metrics(pc)`` with the global
    prometheus_client module, which registers into the global
    ``REGISTRY``.  Multiple tests calling ``run_http_server`` in the same
    process therefore trip the "Duplicated timeseries" guard.  Patching
    ``_build_metrics`` to forward to ``_fresh_metrics`` (which uses a fresh
    ``CollectorRegistry``) sidesteps the conflict.
    """

    def _builder(pc: Any) -> dict[str, Any]:
        return _fresh_metrics(suffix)

    return _builder


def test_run_http_server_cancelled_error_exits_cleanly(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """run_http_server must swallow ``asyncio.CancelledError`` and close the loop.

    Covers lines 657-665 including the ``except asyncio.CancelledError`` branch.
    """
    monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_CERT", raising=False)
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_KEY", raising=False)

    # Patch _serve to immediately raise CancelledError so the event loop exits.
    async def _fake_serve(port: int, metrics: dict[str, Any]) -> None:
        raise asyncio.CancelledError

    with (
        patch.object(ht, "_serve", side_effect=_fake_serve),
        patch.object(ht, "_build_metrics", side_effect=_isolated_build_metrics("r5_cancel")),
    ):
        # Must not raise.
        ht.run_http_server(port=0, log_level="WARNING")


def test_run_http_server_keyboard_interrupt_exits_cleanly(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """run_http_server must swallow ``KeyboardInterrupt`` and close the loop.

    Covers lines 661-662 in the ``except KeyboardInterrupt`` branch.
    """
    monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_CERT", raising=False)
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_KEY", raising=False)

    async def _fake_serve(port: int, metrics: dict[str, Any]) -> None:
        raise KeyboardInterrupt

    with (
        patch.object(ht, "_serve", side_effect=_fake_serve),
        patch.object(ht, "_build_metrics", side_effect=_isolated_build_metrics("r5_kbi")),
    ):
        ht.run_http_server(port=0, log_level="WARNING")


def test_run_http_server_closes_loop_in_finally(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """run_http_server must always close the event loop in its ``finally`` block.

    Validates the ``loop.close()`` call on the ``finally`` path (line 665).
    """
    monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_CERT", raising=False)
    monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_KEY", raising=False)

    closed_loops: list[bool] = []
    original_new_event_loop = asyncio.new_event_loop

    def _patched_new_event_loop() -> asyncio.AbstractEventLoop:
        loop = original_new_event_loop()
        original_close = loop.close

        def _patched_close() -> None:
            closed_loops.append(True)
            original_close()

        loop.close = _patched_close  # type: ignore[method-assign]
        return loop

    async def _fake_serve(port: int, metrics: dict[str, Any]) -> None:
        raise asyncio.CancelledError

    with (
        patch("asyncio.new_event_loop", side_effect=_patched_new_event_loop),
        patch.object(ht, "_serve", side_effect=_fake_serve),
        patch.object(ht, "_build_metrics", side_effect=_isolated_build_metrics("r5_finally")),
    ):
        ht.run_http_server(port=0, log_level="WARNING")

    assert closed_loops, "event loop was not closed in the finally block"


# ---------------------------------------------------------------------------
# make_score_handler: factory return (line 733)
# ---------------------------------------------------------------------------


def test_make_score_handler_returns_callable() -> None:
    """make_score_handler must return an async callable bound to the given metrics."""
    import inspect

    metrics = _fresh_metrics("r5_handler_factory")
    handler = ht.make_score_handler(metrics)

    # The returned handler must be a callable (coroutine function).
    assert callable(handler)
    assert inspect.iscoroutinefunction(handler)


@pytest.mark.asyncio
async def test_make_score_handler_delegates_to_handle_score(
    no_auth_client: TestClient, tmp_path: Path
) -> None:
    """The handler returned by make_score_handler must delegate to _handle_score.

    This exercises line 733 (the ``return _handler`` statement) by constructing
    the handler via the factory and verifying it wires metrics + request
    correctly through to the 200 success path.
    """
    import vmaf_mcp.server as _srv

    unique = id(test_make_score_handler_delegates_to_handle_score)
    metrics = _fresh_metrics(f"r5_factory_delegate_{unique}")
    handler = ht.make_score_handler(metrics)

    ref = tmp_path / "r.yuv"
    dis = tmp_path / "d.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    fake_result = {"vmaf": 77.7, "pooled_metrics": {}, "frames": []}

    with (
        patch.object(_srv, "_validate_path", side_effect=Path),
        patch.object(_srv, "_run_vmaf_score", new=AsyncMock(return_value=fake_result)),
    ):
        # Build a fresh app using the factory handler directly.
        app = aiohttp.web.Application(
            middlewares=[ht._make_security_middleware()],
            client_max_size=ht.MAX_REQUEST_BODY_BYTES,
        )
        app.router.add_post("/v1/score", handler)
        from aiohttp.test_utils import TestServer

        async with (
            TestServer(app) as server,
            aiohttp.ClientSession() as session,
        ):
            env_override = {"VMAFX_MCP_HTTP_NO_AUTH": "1"}
            with patch.dict(os.environ, env_override, clear=False):
                async with session.post(
                    server.make_url("/v1/score"),
                    json={
                        "reference": str(ref),
                        "distorted": str(dis),
                        "width": 1920,
                        "height": 1080,
                        "pixfmt": "420",
                        "bitdepth": 8,
                    },
                ) as resp:
                    assert resp.status == 200
                    body = await resp.json()
                    assert body["vmaf"] == 77.7
                    assert "request_id" in body
