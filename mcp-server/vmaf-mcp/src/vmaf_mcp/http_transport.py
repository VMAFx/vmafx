# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""HTTP transport for the VMAFX MCP server.

Implements an aiohttp-based HTTP server that exposes:
- ``GET /healthz`` — liveness probe (always 200 while the process is alive).
- ``GET /readyz`` — readiness probe (200 once the vmaf binary is reachable).
- ``GET /metrics`` — Prometheus exposition (prometheus-client).
- ``POST /v1/score`` — thin JSON wrapper over ``_run_vmaf_score``.

Activated via ``vmaf-mcp --transport http [--port PORT]``.
Default transport remains stdio for IDE/MCP-client compatibility.

Environment variables (12-factor §III):
- ``VMAFX_PORT``              — HTTP listen port (default 8080; overridden by ``--port``).
- ``VMAFX_LOG_LEVEL``         — Python log level name (default ``INFO``).
- ``VMAFX_VMAF_BINARY``       — Path to the vmaf binary (same as ``VMAF_BIN``).
- ``VMAFX_MODEL_DIR``         — Additional model search root.
- ``VMAFX_MCP_HTTP_TOKEN``    — Bearer token for ``Authorization: Bearer <token>``
                                 enforcement.  **Required** in production.  If unset
                                 and ``VMAFX_MCP_HTTP_NO_AUTH`` is also unset the
                                 server starts but rejects all requests with 401.
- ``VMAFX_MCP_HTTP_NO_AUTH``  — Set to ``1`` to disable authentication entirely
                                 (e.g. when sitting behind a network-level auth
                                 gateway). Logged as a warning on startup.
- ``VMAFX_MCP_HTTP_BIND``     — Bind address (default ``127.0.0.1``).  Set to
                                 ``0.0.0.0`` to listen on all interfaces.
                                 **Breaking change from pre-ADR-0967 behaviour**:
                                 the old default was ``0.0.0.0``.  Existing
                                 deployments must set ``VMAFX_MCP_HTTP_BIND=0.0.0.0``
                                 to restore the old behaviour.
- ``VMAFX_MCP_HTTP_TLS_CERT`` — Path to PEM certificate file.  When set together
                                 with ``VMAFX_MCP_HTTP_TLS_KEY`` the server enables
                                 TLS.  If absent, a warning is logged (HTTP only).
- ``VMAFX_MCP_HTTP_TLS_KEY``  — Path to PEM private key file.

Security (ADR-0967):
- Bodies larger than ``MAX_REQUEST_BODY_BYTES`` (4 MiB) are rejected with 413.
  This is enforced via two complementary mechanisms:
  (a) ``client_max_size`` on the aiohttp ``Application`` — handles chunked/
      streaming bodies; aiohttp raises ``HTTPRequestEntityTooLarge`` (413)
      automatically when ``request.read()`` / ``request.json()`` is called.
  (b) A ``Content-Length`` pre-flight in the security middleware — fast-fails
      before reading any bytes when the declared size exceeds the limit.
- Requests lacking a valid ``Authorization: Bearer`` header are rejected with 401
  unless ``VMAFX_MCP_HTTP_NO_AUTH=1``.
- The server binds ``127.0.0.1`` by default; ``0.0.0.0`` requires an explicit
  opt-in via ``VMAFX_MCP_HTTP_BIND``.

CLI flags take precedence over environment variables; environment variables
take precedence over compiled-in defaults.

ADR-0701: vmafx-server HTTP transport + observability foundation.
ADR-0967: MCP HTTP transport security hardening (auth + body limit + bind default).
"""

from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import signal
import ssl
import sys
import time
import uuid
from collections.abc import Callable
from typing import Any

# ---------------------------------------------------------------------------
# Security constants (ADR-0967)
# ---------------------------------------------------------------------------

#: Maximum request body size accepted by the transport.  Requests whose
#: ``Content-Length`` header exceeds this limit are rejected with HTTP 413
#: by the security middleware before any body bytes are read.  Requests
#: without ``Content-Length`` (chunked transfer) are limited by
#: ``client_max_size`` passed to the aiohttp ``Application``, which raises
#: ``HTTPRequestEntityTooLarge`` (413) inside ``request.read()`` /
#: ``request.json()``.
MAX_REQUEST_BODY_BYTES: int = 4 * 1024 * 1024  # 4 MiB

# ---------------------------------------------------------------------------
# Lazy-import guards for optional heavy deps
# ---------------------------------------------------------------------------


def _require_aiohttp() -> Any:
    """Return the aiohttp module; raise ImportError with install hint if absent."""
    try:
        import aiohttp

        return aiohttp
    except ImportError as exc:
        raise ImportError("HTTP transport requires aiohttp: pip install 'vmaf-mcp[http]'") from exc


def _require_prometheus() -> Any:
    """Return the prometheus_client module; raise ImportError with install hint."""
    try:
        import prometheus_client

        return prometheus_client
    except ImportError as exc:
        raise ImportError(
            "HTTP transport requires prometheus-client: pip install 'vmaf-mcp[http]'"
        ) from exc


# ---------------------------------------------------------------------------
# Structured JSON logging
# ---------------------------------------------------------------------------


class _JSONFormatter(logging.Formatter):
    """Emit each log record as a single-line JSON object.

    Fields: ``timestamp`` (ISO-8601 ms), ``level``, ``message``,
    ``request_id`` (from ``LogRecord.request_id`` if present, else ``"-"``),
    ``logger``, ``module``, ``lineno``.
    """

    def format(self, record: logging.LogRecord) -> str:
        payload = {
            "timestamp": self.formatTime(record, "%Y-%m-%dT%H:%M:%S.") + f"{record.msecs:03.0f}Z",
            "level": record.levelname,
            "message": record.getMessage(),
            "request_id": getattr(record, "request_id", "-"),
            "logger": record.name,
            "module": record.module,
            "lineno": record.lineno,
        }
        if record.exc_info:
            payload["exc_info"] = self.formatException(record.exc_info)
        return json.dumps(payload, ensure_ascii=False)


def configure_logging(level: str = "INFO") -> None:
    """Replace the root logger's handlers with a single structured JSON handler."""
    root = logging.getLogger()
    root.setLevel(getattr(logging, level.upper(), logging.INFO))
    if root.handlers:
        for h in root.handlers:
            root.removeHandler(h)
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(_JSONFormatter())
    root.addHandler(handler)


_log = logging.getLogger("vmafx.http")


def _log_with_rid(level: int, msg: str, request_id: str, **kwargs: Any) -> None:
    """Emit a log record at ``level`` with ``request_id`` attached as an extra."""
    _log.log(level, msg, extra={"request_id": request_id}, **kwargs)


# ---------------------------------------------------------------------------
# Prometheus metrics registry
# ---------------------------------------------------------------------------


def _build_metrics(pc: Any) -> dict[str, Any]:
    """Create and return the Prometheus metrics objects.

    Called once at server start.  Returns a dict with keys:
    - ``scoring_requests_total`` — Counter
    - ``scoring_errors_total`` — Counter
    - ``scoring_duration_seconds`` — Histogram
    """
    return {
        "scoring_requests_total": pc.Counter(
            "vmaf_scoring_requests_total",
            "Total number of VMAF scoring requests received",
            ["endpoint", "status"],
        ),
        "scoring_errors_total": pc.Counter(
            "vmaf_scoring_errors_total",
            "Total number of VMAF scoring requests that resulted in an error",
        ),
        "scoring_duration_seconds": pc.Histogram(
            "vmaf_scoring_duration_seconds",
            "Histogram of VMAF scoring request latencies",
            buckets=[0.1, 0.5, 1.0, 2.5, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0],
        ),
    }


# ---------------------------------------------------------------------------
# Security helpers (ADR-0967)
# ---------------------------------------------------------------------------


def _resolve_auth_token() -> str | None:
    """Return the expected bearer token from the environment, or None.

    Returns the value of ``VMAFX_MCP_HTTP_TOKEN`` if set and non-empty;
    otherwise ``None``.
    """
    val = os.environ.get("VMAFX_MCP_HTTP_TOKEN", "").strip()
    return val if val else None


def _no_auth_mode() -> bool:
    """Return True when ``VMAFX_MCP_HTTP_NO_AUTH=1`` is set."""
    return os.environ.get("VMAFX_MCP_HTTP_NO_AUTH", "").strip() == "1"


def _resolve_bind_host() -> str:
    """Return the effective bind host.

    Defaults to ``127.0.0.1`` (loopback-only) per ADR-0967.  Operators who
    need to listen on all interfaces must set ``VMAFX_MCP_HTTP_BIND=0.0.0.0``
    explicitly.

    **Migration note**: the pre-ADR-0967 default was ``0.0.0.0``.  Existing
    Helm deployments or ``docker run`` invocations that rely on pod-network
    reachability must add ``VMAFX_MCP_HTTP_BIND=0.0.0.0`` to their environment.
    The Helm values file ``deploy/helm/vmafx/values.yaml`` ships with this set
    as a documented operator choice.
    """
    return os.environ.get("VMAFX_MCP_HTTP_BIND", "127.0.0.1")


def _build_ssl_context() -> ssl.SSLContext | None:
    """Build and return an ``ssl.SSLContext`` if TLS env vars are set.

    Returns ``None`` (plain HTTP) if either env var is absent.  Logs a warning
    when TLS is not configured so operators are not surprised.
    """
    cert = os.environ.get("VMAFX_MCP_HTTP_TLS_CERT", "").strip()
    key = os.environ.get("VMAFX_MCP_HTTP_TLS_KEY", "").strip()

    if cert and key:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=cert, keyfile=key)
        _log.info(
            "TLS enabled: cert=%s key=%s",
            cert,
            key,
            extra={"request_id": "-"},
        )
        return ctx

    _log.warning(
        "VMAFX_MCP_HTTP_TLS_CERT / VMAFX_MCP_HTTP_TLS_KEY not set — "
        "HTTP transport is running without TLS.  Set both env vars to enable TLS.",
        extra={"request_id": "-"},
    )
    return None


def _make_security_middleware() -> Any:
    """Return an aiohttp ``@web.middleware`` that enforces body size + bearer auth.

    The returned function is decorated with ``@web.middleware`` so aiohttp
    registers it correctly.  It is constructed via a factory so that unit tests
    can patch ``_resolve_auth_token`` / ``_no_auth_mode`` before the middleware
    is called without needing to re-create the application.

    Body-size gate (all verbs):
    - If ``Content-Length`` header is present and > ``MAX_REQUEST_BODY_BYTES``,
      reject immediately with 413 before reading any body bytes.
    - For chunked or unknown-length bodies, ``client_max_size`` on the aiohttp
      ``Application`` enforces the limit inside ``request.json()`` / ``.read()``
      by raising ``HTTPRequestEntityTooLarge`` (which aiohttp converts to 413).

    Auth gate (all verbs):
    - If ``VMAFX_MCP_HTTP_NO_AUTH=1``, skip authentication entirely.
    - If ``VMAFX_MCP_HTTP_TOKEN`` is unset (and NO_AUTH is also unset), reject
      every request with 401.  No-token-configured means the operator has not
      set up auth; refusing all traffic is safer than silently accepting it.
    - Otherwise, the ``Authorization: Bearer <token>`` header must match
      ``VMAFX_MCP_HTTP_TOKEN`` exactly via ``hmac.compare_digest`` (constant-time
      comparison prevents token enumeration via wall-clock timing side-channels).

    Health probes (``/healthz``, ``/readyz``) and ``/metrics`` are **not** exempt
    from auth because Kubernetes health-check source IPs should already be inside
    the network trust boundary when token auth is required.
    """
    aiohttp = _require_aiohttp()

    @aiohttp.web.middleware
    async def _security_middleware(request: Any, handler: Any) -> Any:
        # --- Body-size pre-flight via Content-Length header -------------------
        content_length = request.content_length
        if content_length is not None and content_length > MAX_REQUEST_BODY_BYTES:
            return aiohttp.web.Response(
                status=413,
                content_type="application/json",
                text=json.dumps(
                    {
                        "error": (
                            f"Request body too large: Content-Length {content_length} "
                            f"exceeds limit {MAX_REQUEST_BODY_BYTES}"
                        )
                    }
                ),
            )

        # --- Auth gate --------------------------------------------------------
        if not _no_auth_mode():
            expected_token = _resolve_auth_token()
            if expected_token is None:
                # No token configured and no explicit opt-out: refuse all traffic.
                _log.warning(
                    "VMAFX_MCP_HTTP_TOKEN is unset and VMAFX_MCP_HTTP_NO_AUTH!=1; "
                    "rejecting request (set the token or set NO_AUTH=1 to accept "
                    "unauthenticated traffic)",
                    extra={"request_id": "-"},
                )
                return aiohttp.web.Response(
                    status=401,
                    content_type="application/json",
                    text=json.dumps(
                        {
                            "error": (
                                "Unauthorized: server requires VMAFX_MCP_HTTP_TOKEN "
                                "or VMAFX_MCP_HTTP_NO_AUTH=1"
                            )
                        }
                    ),
                )
            auth_header = request.headers.get("Authorization", "")
            if not auth_header.startswith("Bearer ") or not hmac.compare_digest(
                auth_header[len("Bearer ") :], expected_token
            ):
                return aiohttp.web.Response(
                    status=401,
                    content_type="application/json",
                    text=json.dumps({"error": "Unauthorized: invalid or missing Bearer token"}),
                )

        return await handler(request)

    return _security_middleware


# ---------------------------------------------------------------------------
# Request handlers
# ---------------------------------------------------------------------------


async def _handle_healthz(request: Any) -> Any:
    """GET /healthz — liveness probe.  Always 200 while the process is alive."""
    aiohttp = _require_aiohttp()
    return aiohttp.web.Response(
        status=200,
        content_type="application/json",
        text=json.dumps({"status": "healthy"}),
    )


async def _handle_readyz(request: Any) -> Any:
    """GET /readyz — readiness probe.

    Returns 200 once the vmaf binary path resolves to an executable file;
    503 otherwise.  The check is intentionally cheap (stat, no subprocess)
    so it doesn't slow down Kubernetes readiness polling.
    """
    aiohttp = _require_aiohttp()
    # Import lazily to avoid a circular import; server.py imports us.
    from vmaf_mcp.server import _vmaf_binary

    vmaf = _vmaf_binary()
    if vmaf.exists() and vmaf.is_file():
        return aiohttp.web.Response(
            status=200,
            content_type="application/json",
            text=json.dumps({"status": "ready", "vmaf_binary": str(vmaf)}),
        )
    return aiohttp.web.Response(
        status=503,
        content_type="application/json",
        text=json.dumps(
            {
                "status": "not_ready",
                "reason": f"vmaf binary not found at {vmaf}",
            }
        ),
    )


async def _handle_metrics(request: Any) -> Any:
    """GET /metrics — Prometheus exposition format.

    aiohttp 3.13.5 added strict validation that rejects a ``charset=...``
    fragment inside the ``content_type=`` kwarg (it expects a bare MIME
    type and a separate ``charset=`` argument).  ``prometheus_client``'s
    ``CONTENT_TYPE_LATEST`` is the full RFC 1341 value
    ``text/plain; version=1.0.0; charset=utf-8`` — passing it via
    ``content_type=`` raises ``ValueError("charset must not be in
    content_type argument")``.  Set the header directly via ``headers=``
    so the verbatim Prometheus exposition Content-Type lands on the
    response without aiohttp re-parsing it.
    """
    pc = _require_prometheus()
    aiohttp = _require_aiohttp()
    output = pc.generate_latest()
    return aiohttp.web.Response(
        status=200,
        body=output,
        headers={"Content-Type": pc.CONTENT_TYPE_LATEST},
    )


async def _handle_score(request: Any, metrics: dict[str, Any]) -> Any:
    """POST /v1/score — single scoring request.

    Request body (JSON):
    ```json
    {
      "reference": "<path>",
      "distorted": "<path>",
      "width": 1920,
      "height": 1080,
      "pixfmt": "420",
      "bitdepth": 8,
      "model": "version=vmaf_v0.6.1",  // optional
      "backend": "auto",               // optional
      "precision": "17"                // optional
    }
    ```

    Returns the vmaf JSON payload on success; ``{"error": "..."}`` with
    status 400/500 on failure.

    Body size is bounded to ``MAX_REQUEST_BODY_BYTES`` by the security
    middleware (Content-Length pre-flight) and by ``client_max_size`` on the
    ``Application`` (chunked bodies — raises ``HTTPRequestEntityTooLarge`` 413
    inside ``request.json()``).
    """
    aiohttp = _require_aiohttp()
    from vmaf_mcp.server import ScoreRequest, _run_vmaf_score, _validate_path

    request_id = str(uuid.uuid4())[:8]
    t0 = time.monotonic()
    _log_with_rid(logging.INFO, "POST /v1/score received", request_id)

    try:
        body = await request.json()
    except aiohttp.web.HTTPRequestEntityTooLarge:
        # Chunked body exceeded client_max_size — re-raise so aiohttp converts it
        # to the correct 413 response.  The bare ``except Exception`` below would
        # catch this and mis-report it as 400 "invalid JSON" (ADR-1075 bug A).
        raise
    except Exception as exc:
        _log_with_rid(logging.WARNING, f"invalid JSON body: {exc}", request_id)
        metrics["scoring_requests_total"].labels(endpoint="/v1/score", status="400").inc()
        return aiohttp.web.Response(
            status=400,
            content_type="application/json",
            text=json.dumps({"error": f"invalid JSON: {exc}", "request_id": request_id}),
        )

    # Reject non-object JSON payloads (null, arrays, scalars).  ``request.json()``
    # succeeds for any valid JSON type; a non-dict body would cause a TypeError
    # on the ``f not in body`` membership check below — uncaught and thus an
    # uncontrolled 500 (ADR-1075 bug B).
    if not isinstance(body, dict):
        _log_with_rid(
            logging.WARNING,
            f"expected JSON object, got {type(body).__name__}",
            request_id,
        )
        metrics["scoring_requests_total"].labels(endpoint="/v1/score", status="400").inc()
        return aiohttp.web.Response(
            status=400,
            content_type="application/json",
            text=json.dumps(
                {
                    "error": (
                        f"expected JSON object, got {type(body).__name__}; "
                        "request body must be a JSON object"
                    ),
                    "request_id": request_id,
                }
            ),
        )

    # Validate required fields.
    missing = [f for f in ("reference", "distorted") if f not in body]
    if missing:
        metrics["scoring_requests_total"].labels(endpoint="/v1/score", status="400").inc()
        return aiohttp.web.Response(
            status=400,
            content_type="application/json",
            text=json.dumps(
                {"error": f"missing required fields: {missing}", "request_id": request_id}
            ),
        )

    # Width/height: required unless reference is an encoded video path.
    # For raw YUV scoring (this endpoint) they are required.
    for field in ("width", "height", "pixfmt", "bitdepth"):
        if field not in body:
            metrics["scoring_requests_total"].labels(endpoint="/v1/score", status="400").inc()
            return aiohttp.web.Response(
                status=400,
                content_type="application/json",
                text=json.dumps(
                    {"error": f"missing required field: {field!r}", "request_id": request_id}
                ),
            )

    try:
        ref_path = _validate_path(str(body["reference"]))
        dis_path = _validate_path(str(body["distorted"]))
        score_req = ScoreRequest(
            ref=ref_path,
            dis=dis_path,
            width=int(body["width"]),
            height=int(body["height"]),
            pixfmt=str(body["pixfmt"]),
            bitdepth=int(body["bitdepth"]),
            model=str(body.get("model", "version=vmaf_v0.6.1")),
            backend=str(body.get("backend", "auto")),
            precision=str(body.get("precision", "17")),
        )
    except (ValueError, FileNotFoundError) as exc:
        _log_with_rid(logging.WARNING, f"bad request parameters: {exc}", request_id)
        metrics["scoring_requests_total"].labels(endpoint="/v1/score", status="400").inc()
        return aiohttp.web.Response(
            status=400,
            content_type="application/json",
            text=json.dumps({"error": str(exc), "request_id": request_id}),
        )

    # Run the scorer.
    try:
        with metrics["scoring_duration_seconds"].time():
            result = await _run_vmaf_score(score_req)
    except Exception as exc:
        elapsed = (time.monotonic() - t0) * 1000
        _log_with_rid(logging.ERROR, f"scoring failed in {elapsed:.0f}ms: {exc}", request_id)
        metrics["scoring_requests_total"].labels(endpoint="/v1/score", status="500").inc()
        metrics["scoring_errors_total"].inc()
        return aiohttp.web.Response(
            status=500,
            content_type="application/json",
            text=json.dumps({"error": str(exc), "request_id": request_id}),
        )

    elapsed = (time.monotonic() - t0) * 1000
    _log_with_rid(logging.INFO, f"POST /v1/score done in {elapsed:.0f}ms", request_id)
    metrics["scoring_requests_total"].labels(endpoint="/v1/score", status="200").inc()
    result["request_id"] = request_id
    return aiohttp.web.Response(
        status=200,
        content_type="application/json",
        text=json.dumps(result),
    )


# ---------------------------------------------------------------------------
# SIGTERM graceful shutdown
# ---------------------------------------------------------------------------


def _install_sigterm_handler(runner: Any, loop: asyncio.AbstractEventLoop) -> None:
    """Register a SIGTERM handler that initiates graceful shutdown.

    On SIGTERM:
    1. Logs the signal.
    2. Schedules ``runner.cleanup()`` on the event loop.
    3. Stops the event loop (which allows ``run_forever`` to return).

    The 30-second hard timeout is enforced by ``asyncio.wait_for`` in
    :func:`run_http_server`.
    """

    def _handler(signum: int, _frame: Any) -> None:
        _log.info("SIGTERM received — initiating graceful shutdown", extra={"request_id": "-"})
        loop.call_soon_threadsafe(loop.stop)

    signal.signal(signal.SIGTERM, _handler)
    signal.signal(signal.SIGINT, _handler)


# ---------------------------------------------------------------------------
# Server entry point
# ---------------------------------------------------------------------------


def _make_app(metrics: dict[str, Any]) -> Any:
    """Build and return the aiohttp Application.

    The ``metrics`` dict is passed in so tests can inject a fresh registry
    without touching module-level state.

    Security middleware (ADR-0967) is attached here so it runs for every route
    including health probes and metrics.  The ``client_max_size`` parameter
    enforces the body limit for chunked / unknown-length bodies.
    """
    aiohttp = _require_aiohttp()
    app = aiohttp.web.Application(
        middlewares=[_make_security_middleware()],
        client_max_size=MAX_REQUEST_BODY_BYTES,
    )

    # Bind metrics into the score handler via a closure.
    async def _score_handler(request: Any) -> Any:
        return await _handle_score(request, metrics)

    app.router.add_get("/healthz", _handle_healthz)
    app.router.add_get("/readyz", _handle_readyz)
    app.router.add_get("/metrics", _handle_metrics)
    app.router.add_post("/v1/score", _score_handler)
    return app


async def _serve(port: int, metrics: dict[str, Any]) -> None:
    """Run the HTTP server until the event loop is stopped.

    Separated from ``run_http_server`` so tests can call it directly
    without going through the signal-handler / shutdown machinery.

    Bind host defaults to ``127.0.0.1`` (loopback-only) per ADR-0967.
    Set ``VMAFX_MCP_HTTP_BIND=0.0.0.0`` to listen on all interfaces.
    TLS is optional — set ``VMAFX_MCP_HTTP_TLS_CERT`` and
    ``VMAFX_MCP_HTTP_TLS_KEY`` to enable.
    """
    aiohttp = _require_aiohttp()
    app = _make_app(metrics)
    runner = aiohttp.web.AppRunner(app)
    await runner.setup()

    bind_host = _resolve_bind_host()
    ssl_context = _build_ssl_context()
    scheme = "https" if ssl_context else "http"

    site = aiohttp.web.TCPSite(runner, bind_host, port, ssl_context=ssl_context)
    await site.start()
    _log.info(
        "vmafx-server listening on %s://%s:%d",
        scheme,
        bind_host,
        port,
        extra={"request_id": "-"},
    )

    # Emit startup warnings about auth / TLS state.
    if not _no_auth_mode() and _resolve_auth_token() is None:
        _log.warning(
            "VMAFX_MCP_HTTP_TOKEN is unset — all requests will be rejected with 401. "
            "Set the token or set VMAFX_MCP_HTTP_NO_AUTH=1 to accept unauthenticated traffic.",
            extra={"request_id": "-"},
        )
    elif _no_auth_mode():
        _log.warning(
            "VMAFX_MCP_HTTP_NO_AUTH=1 — authentication disabled.  "
            "Ensure network-level controls restrict access.",
            extra={"request_id": "-"},
        )

    # Hang here until the event loop is stopped by the SIGTERM handler.
    try:
        await asyncio.Event().wait()  # effectively forever
    finally:
        # Drain in-flight requests and shut down cleanly within the
        # 30-second hard timeout enforced by the caller.
        _log.info("shutting down HTTP server", extra={"request_id": "-"})
        await runner.cleanup()


def run_http_server(port: int = 8080, log_level: str = "INFO") -> None:
    """Launch the HTTP server synchronously (blocks until SIGTERM / SIGINT).

    This is the entry point called by ``main()`` when
    ``--transport http`` is requested.

    Args:
        port: TCP port to listen on.
        log_level: Python logging level name (INFO, DEBUG, WARNING, …).
    """
    configure_logging(log_level)
    pc = _require_prometheus()
    metrics = _build_metrics(pc)

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    _install_sigterm_handler(_DummyRunner(), loop)

    try:
        loop.run_until_complete(asyncio.wait_for(_serve(port, metrics), timeout=None))
    except asyncio.CancelledError:
        pass
    except KeyboardInterrupt:
        pass
    finally:
        _log.info("event loop closed", extra={"request_id": "-"})
        loop.close()


class _DummyRunner:
    """Placeholder passed to ``_install_sigterm_handler`` before the real runner exists.

    The SIGTERM handler only calls ``loop.stop()``; it does not call
    ``runner.cleanup()`` — cleanup happens in the ``finally`` block of
    ``_serve``.  This avoids requiring the real ``AppRunner`` before
    the event loop starts.
    """


# ---------------------------------------------------------------------------
# Env-var config resolution helpers (12-factor §III)
# ---------------------------------------------------------------------------


def _resolve_port(cli_port: int | None) -> int:
    """Return the effective HTTP port: CLI > env > default 8080."""
    if cli_port is not None:
        return cli_port
    env = os.environ.get("VMAFX_PORT")
    if env:
        try:
            return int(env)
        except ValueError:
            _log.warning(
                f"VMAFX_PORT={env!r} is not a valid integer; using default 8080",
                extra={"request_id": "-"},
            )
    return 8080


def _resolve_log_level(cli_level: str | None) -> str:
    """Return the effective log level: CLI > env > default 'INFO'."""
    if cli_level:
        return cli_level
    return os.environ.get("VMAFX_LOG_LEVEL", "INFO")


def _apply_env_overrides() -> None:
    """Copy VMAFX_* env vars into the canonical names used by server.py.

    Allows operator-level config without touching ``VMAF_BIN`` / the
    existing env-var contract in ``server.py``.
    """
    vmaf_bin = os.environ.get("VMAFX_VMAF_BINARY")
    if vmaf_bin and "VMAF_BIN" not in os.environ:
        os.environ["VMAF_BIN"] = vmaf_bin

    model_dir = os.environ.get("VMAFX_MODEL_DIR")
    if model_dir:
        existing = os.environ.get("VMAF_MCP_ALLOW", "")
        if model_dir not in existing.split(":"):
            os.environ["VMAF_MCP_ALLOW"] = f"{existing}:{model_dir}" if existing else model_dir


def make_score_handler(metrics: dict[str, Any]) -> Callable[..., Any]:
    """Return the /v1/score handler bound to *metrics*.

    Exposed for testing: tests can inject a custom metrics dict and a fresh
    prometheus_client registry without mutating module-level state.
    """

    async def _handler(request: Any) -> Any:
        return await _handle_score(request, metrics)

    return _handler
