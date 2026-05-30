"""HTTP transport for the VMAFX MCP server.

Implements an aiohttp-based HTTP server that exposes:
- ``GET /healthz`` — liveness probe (always 200 while the process is alive).
- ``GET /readyz`` — readiness probe (200 once the vmaf binary is reachable).
- ``GET /metrics`` — Prometheus exposition (prometheus-client).
- ``POST /v1/score`` — thin JSON wrapper over ``_run_vmaf_score``.

Activated via ``vmaf-mcp --transport http [--port PORT]``.
Default transport remains stdio for IDE/MCP-client compatibility.

Environment variables (12-factor §III):
- ``VMAFX_PORT``       — HTTP listen port (default 8080; overridden by ``--port``).
- ``VMAFX_LOG_LEVEL``  — Python log level name (default ``INFO``).
- ``VMAFX_VMAF_BINARY`` — Path to the vmaf binary (same as ``VMAF_BIN``).
- ``VMAFX_MODEL_DIR``   — Additional model search root.

CLI flags take precedence over environment variables; environment variables
take precedence over compiled-in defaults.

ADR-0701: vmafx-server HTTP transport + observability foundation.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import sys
import time
import uuid
from collections.abc import Callable
from typing import Any

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
    """GET /metrics — Prometheus exposition format."""
    pc = _require_prometheus()
    aiohttp = _require_aiohttp()
    output = pc.generate_latest()
    return aiohttp.web.Response(
        status=200,
        content_type=pc.CONTENT_TYPE_LATEST,
        body=output,
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
    """
    aiohttp = _require_aiohttp()
    from vmaf_mcp.server import ScoreRequest, _run_vmaf_score, _validate_path

    request_id = str(uuid.uuid4())[:8]
    t0 = time.monotonic()
    _log_with_rid(logging.INFO, "POST /v1/score received", request_id)

    try:
        body = await request.json()
    except Exception as exc:
        _log_with_rid(logging.WARNING, f"invalid JSON body: {exc}", request_id)
        metrics["scoring_requests_total"].labels(endpoint="/v1/score", status="400").inc()
        return aiohttp.web.Response(
            status=400,
            content_type="application/json",
            text=json.dumps({"error": f"invalid JSON: {exc}", "request_id": request_id}),
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
    """
    aiohttp = _require_aiohttp()
    app = aiohttp.web.Application()

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
    """
    aiohttp = _require_aiohttp()
    app = _make_app(metrics)
    runner = aiohttp.web.AppRunner(app)
    await runner.setup()
    site = aiohttp.web.TCPSite(runner, "0.0.0.0", port)
    await site.start()
    _log.info(
        f"vmafx-server listening on http://0.0.0.0:{port}",
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
