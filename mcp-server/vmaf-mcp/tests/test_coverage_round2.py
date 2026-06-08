# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Round-2 coverage uplift for vmaf-mcp (post PR #346).

This module pushes the Python coverage of ``mcp-server/vmaf-mcp/src/vmaf_mcp/``
beyond PR #346's +17pp lift (61 -> 78 %). It targets the residual gaps that
**neither** PR #305 (server.py error-path hardening) **nor** PR #346 (isError
invariant suite + http_transport endpoint coverage) own:

http_transport.py — infrastructure helpers untouched by PR #346's smoke layer:
- ``_JSONFormatter`` log-record shaping (with and without ``exc_info``).
- ``configure_logging`` handler-replacement contract.
- ``_resolve_port`` / ``_resolve_log_level`` precedence (CLI > env > default,
  including malformed-env fallback).
- ``_apply_env_overrides`` env-name mapping into the canonical
  ``VMAF_BIN`` / ``VMAF_MCP_ALLOW`` slots used by ``server.py``.
- ``make_score_handler`` closure binding (delegates to ``_handle_score``).
- ``_install_sigterm_handler`` registers without raising (no signal fired).
- ``_DummyRunner`` is constructible (used during early SIGTERM wiring).
- ``_require_aiohttp`` / ``_require_prometheus`` ImportError branch.
- ``/v1/score`` validation edges (missing width/height/pixfmt/bitdepth, bad
  type, scorer raising).

server.py — residual helpers untouched by PR #305 / PR #346:
- ``_classify_source_resolution`` 4K bucket branch.
- ``_resolution_mismatch_warning`` all branches (None / match / mismatch).
- ``_describe_model_file`` JSONDecodeError swallow branch.
- ``_decode_to_yuv`` ffmpeg-missing + non-zero-exit raise paths.
- ``_ffprobe_geometry`` ffprobe failure + no-video-stream branches.
- ``_run_vmaf_score`` non-zero-exit raise.
- ``_run_vmaf_score`` explicit-backend-not-advertised raise (Bug #1 echo).
- ``_run_vmaf_score_encoded`` integration via stubbed ``_run_vmaf_score``.
- ``_load_vlm`` returns None when transformers/torch absent.
- ``_vmaf_version`` --version timeout fall-through.
- ``_call_tool`` success-path dispatch for the under-exercised tools
  (``list_backends``, ``vmaf_version``, ``list_extractors``, ``describe_model``,
  ``probe_backend``).
- ``main`` chooses the configured anyio backend.
- ``_run_compare`` non-JSON stdout fallback.

No overlap with:
- ``tests/test_iserror_invariant.py`` (PR #346) — that file walks the *failure*
  paths (raise -> isError=True). Round-2 covers helper internals and *success*
  paths.
- PR #305 — that PR only modifies ``server.py`` source; no tests added.

ADR-0108 deliverables for this PR are captured in:
- Research digest: ``docs/research/mcp-server-coverage-round2-20260531.md``.
- Decision matrix: "no alternatives: only-one-way fix" (coverage uplift; each
  target line has one canonical way to exercise it).
- AGENTS.md invariant note: not applicable (no rebase-sensitive surface).
- Reproducer: ``cd mcp-server/vmaf-mcp && python -m pytest tests/test_coverage_round2.py -v``.
- Changelog fragment: ``changelog.d/added/mcp-server-coverage-round2.md``.
- Rebase note: no rebase impact (fork-local Python tests).
"""

from __future__ import annotations

import asyncio
import io
import json
import logging
import os
import signal
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from vmaf_mcp import http_transport as ht
from vmaf_mcp import server as srv

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


@pytest.fixture(autouse=True)
def _clear_probe_cache() -> Iterator[None]:
    """Ensure the backend-probe cache does not leak across tests."""
    srv._BACKEND_PROBE_CACHE.clear()
    yield
    srv._BACKEND_PROBE_CACHE.clear()


# ===========================================================================
# http_transport.py — structured JSON logging
# ===========================================================================


def test_json_formatter_emits_required_fields() -> None:
    """``_JSONFormatter.format`` must emit a single-line JSON object with the
    five documented keys (timestamp/level/message/request_id/logger).
    """
    fmt = ht._JSONFormatter()
    record = logging.LogRecord(
        name="vmafx.test",
        level=logging.INFO,
        pathname=__file__,
        lineno=42,
        msg="hello %s",
        args=("world",),
        exc_info=None,
    )
    line = fmt.format(record)
    payload = json.loads(line)
    assert payload["level"] == "INFO"
    assert payload["message"] == "hello world"
    assert payload["logger"] == "vmafx.test"
    assert payload["request_id"] == "-"  # default when not attached as extra
    assert "timestamp" in payload
    assert payload["lineno"] == 42


def test_json_formatter_propagates_request_id_extra() -> None:
    """When the LogRecord carries a ``request_id`` extra, the formatter must
    surface it instead of the ``"-"`` default.
    """
    fmt = ht._JSONFormatter()
    record = logging.LogRecord(
        name="vmafx.test",
        level=logging.WARNING,
        pathname=__file__,
        lineno=1,
        msg="x",
        args=None,
        exc_info=None,
    )
    record.request_id = "abc12345"  # type: ignore[attr-defined]
    payload = json.loads(fmt.format(record))
    assert payload["request_id"] == "abc12345"


def test_json_formatter_includes_exc_info_when_present() -> None:
    """When ``exc_info`` is set on the record, the formatter must include the
    formatted traceback under the ``exc_info`` key.
    """
    fmt = ht._JSONFormatter()
    try:
        raise RuntimeError("boom for testing")
    except RuntimeError:
        exc_info = sys.exc_info()
    record = logging.LogRecord(
        name="vmafx.test",
        level=logging.ERROR,
        pathname=__file__,
        lineno=1,
        msg="err",
        args=None,
        exc_info=exc_info,
    )
    payload = json.loads(fmt.format(record))
    assert "exc_info" in payload
    assert "RuntimeError" in payload["exc_info"]
    assert "boom for testing" in payload["exc_info"]


def test_configure_logging_replaces_existing_handlers() -> None:
    """``configure_logging`` must (a) drop any pre-existing handlers it sees
    at call time, (b) install a JSON-formatted handler, and (c) honour the
    requested log level. We avoid asserting on the *absolute* handler count
    because pytest's caplog plugin re-attaches its own handler between Python
    statements; we just verify the sentinel handler is gone and the JSON
    handler is present after each call.
    """
    root = logging.getLogger()
    sentinel = logging.StreamHandler(io.StringIO())
    saved_handlers = list(root.handlers)
    saved_level = root.level
    try:
        root.addHandler(sentinel)
        assert sentinel in root.handlers
        ht.configure_logging("DEBUG")
        # Level is honoured.
        assert root.level == logging.DEBUG
        # Sentinel was removed by configure_logging's handler-clear loop.
        assert sentinel not in root.handlers
        # At least one JSON-formatted handler is present.
        json_handlers = [h for h in root.handlers if isinstance(h.formatter, ht._JSONFormatter)]
        assert len(json_handlers) >= 1
        # Calling again at a different level reapplies cleanly.
        ht.configure_logging("WARNING")
        assert root.level == logging.WARNING
        json_handlers = [h for h in root.handlers if isinstance(h.formatter, ht._JSONFormatter)]
        assert len(json_handlers) >= 1
    finally:
        for h in list(root.handlers):
            root.removeHandler(h)
        for h in saved_handlers:
            root.addHandler(h)
        root.setLevel(saved_level)


# ===========================================================================
# http_transport.py — env-var config resolution
# ===========================================================================


def test_resolve_port_cli_wins(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("VMAFX_PORT", "9999")
    assert ht._resolve_port(1234) == 1234


def test_resolve_port_env_when_no_cli(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("VMAFX_PORT", "9090")
    assert ht._resolve_port(None) == 9090


def test_resolve_port_default_when_no_cli_no_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("VMAFX_PORT", raising=False)
    assert ht._resolve_port(None) == 8080


def test_resolve_port_invalid_env_falls_back_to_default(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A malformed ``VMAFX_PORT`` must log a warning and fall back to 8080
    (the documented default in the docstring).
    """
    monkeypatch.setenv("VMAFX_PORT", "not-a-number")
    assert ht._resolve_port(None) == 8080


def test_resolve_log_level_precedence(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("VMAFX_LOG_LEVEL", "DEBUG")
    # CLI wins.
    assert ht._resolve_log_level("ERROR") == "ERROR"
    # Env wins over default.
    assert ht._resolve_log_level(None) == "DEBUG"
    # Default when no env.
    monkeypatch.delenv("VMAFX_LOG_LEVEL", raising=False)
    assert ht._resolve_log_level(None) == "INFO"


def test_apply_env_overrides_maps_vmaf_binary(monkeypatch: pytest.MonkeyPatch) -> None:
    """``VMAFX_VMAF_BINARY`` must seed ``VMAF_BIN`` when ``VMAF_BIN`` is unset.
    Mapping is one-way: existing ``VMAF_BIN`` must NOT be clobbered.
    """
    monkeypatch.delenv("VMAF_BIN", raising=False)
    monkeypatch.setenv("VMAFX_VMAF_BINARY", "/opt/vmaf/bin/vmaf")
    ht._apply_env_overrides()
    assert os.environ["VMAF_BIN"] == "/opt/vmaf/bin/vmaf"


def test_apply_env_overrides_does_not_clobber_vmaf_bin(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("VMAF_BIN", "/pre/existing/vmaf")
    monkeypatch.setenv("VMAFX_VMAF_BINARY", "/should/not/win")
    ht._apply_env_overrides()
    assert os.environ["VMAF_BIN"] == "/pre/existing/vmaf"


def test_apply_env_overrides_appends_model_dir_to_allowlist(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """``VMAFX_MODEL_DIR`` must be appended to (not overwrite) ``VMAF_MCP_ALLOW``,
    and double-application is idempotent (no duplicate colon-segments).
    """
    monkeypatch.setenv("VMAF_MCP_ALLOW", "/already/allowed")
    monkeypatch.setenv("VMAFX_MODEL_DIR", "/extra/model/dir")
    ht._apply_env_overrides()
    assert "/extra/model/dir" in os.environ["VMAF_MCP_ALLOW"]
    assert "/already/allowed" in os.environ["VMAF_MCP_ALLOW"]
    # Second application must NOT duplicate.
    ht._apply_env_overrides()
    assert os.environ["VMAF_MCP_ALLOW"].count("/extra/model/dir") == 1


def test_apply_env_overrides_sets_allow_when_empty(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("VMAF_MCP_ALLOW", raising=False)
    monkeypatch.setenv("VMAFX_MODEL_DIR", "/only/model/dir")
    ht._apply_env_overrides()
    assert os.environ["VMAF_MCP_ALLOW"] == "/only/model/dir"


# ===========================================================================
# http_transport.py — runtime wiring (score handler closure, SIGTERM, dummy)
# ===========================================================================


def test_make_score_handler_returns_callable_bound_to_metrics() -> None:
    """``make_score_handler`` must return a coroutine callable that closes over
    the supplied metrics dict (so tests can inject a fresh prometheus registry).
    """
    import inspect

    metrics: dict[str, Any] = {"sentinel": object()}
    handler = ht.make_score_handler(metrics)
    assert callable(handler)
    # The returned handler is a coroutine function (async def closure).
    assert inspect.iscoroutinefunction(handler)


def test_dummy_runner_is_instantiable() -> None:
    """``_DummyRunner`` is a placeholder that must be constructible without args
    so ``_install_sigterm_handler`` can be called before the real AppRunner
    exists.
    """
    runner = ht._DummyRunner()
    assert runner is not None


def test_install_sigterm_handler_registers_without_raising() -> None:
    """``_install_sigterm_handler`` must successfully install SIGTERM + SIGINT
    handlers and not fire any signal at install time. We restore the originals
    after the test so subsequent tests aren't affected.
    """
    runner = ht._DummyRunner()
    loop = asyncio.new_event_loop()
    orig_term = signal.getsignal(signal.SIGTERM)
    orig_int = signal.getsignal(signal.SIGINT)
    try:
        ht._install_sigterm_handler(runner, loop)
        # Handlers must be installed (different from default).
        term_now = signal.getsignal(signal.SIGTERM)
        int_now = signal.getsignal(signal.SIGINT)
        assert callable(term_now)
        assert callable(int_now)
        # The installed handler must be loop-aware. Invoke directly to cover
        # the inner _handler body (line 330-332) without actually killing
        # the test process.
        assert callable(term_now)
        term_now(signal.SIGTERM, None)  # type: ignore[misc]
        # call_soon_threadsafe schedules loop.stop -- the loop has never run,
        # so stop() is queued but harmless. We just need the function to have
        # executed without raising.
    finally:
        signal.signal(signal.SIGTERM, orig_term)
        signal.signal(signal.SIGINT, orig_int)
        loop.close()


def test_require_aiohttp_raises_helpful_message_when_missing(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """When aiohttp is genuinely missing the helper must surface the documented
    install hint. We simulate by intercepting ``importlib`` for the duration of
    the call.
    """
    real_import = __builtins__["__import__"] if isinstance(__builtins__, dict) else __import__

    def fake_import(name: str, *args: Any, **kwargs: Any) -> Any:
        if name == "aiohttp":
            raise ImportError("simulated")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr("builtins.__import__", fake_import)
    with pytest.raises(ImportError, match="vmaf-mcp\\[http\\]"):
        ht._require_aiohttp()


def test_require_prometheus_raises_helpful_message_when_missing(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Same contract as ``_require_aiohttp`` for the prometheus_client dep."""
    real_import = __builtins__["__import__"] if isinstance(__builtins__, dict) else __import__

    def fake_import(name: str, *args: Any, **kwargs: Any) -> Any:
        if name == "prometheus_client":
            raise ImportError("simulated")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr("builtins.__import__", fake_import)
    with pytest.raises(ImportError, match="vmaf-mcp\\[http\\]"):
        ht._require_prometheus()


# ===========================================================================
# http_transport.py — /v1/score validation edges
# ===========================================================================

aiohttp = pytest.importorskip("aiohttp")
pytest.importorskip("prometheus_client")
pytest_asyncio = pytest.importorskip("pytest_asyncio")

from aiohttp.test_utils import TestClient  # noqa: E402


def _fresh_metrics_for_round2() -> dict[str, Any]:
    """Local copy of the test-only metrics builder so this file is independent
    of ``test_http_transport.py``'s helpers (those use ``_test`` suffixes that
    would collide if both files instantiate them in the same registry).
    """
    import prometheus_client as pc  # type: ignore[import-untyped]

    registry = pc.CollectorRegistry(auto_describe=False)
    return {
        "scoring_requests_total": pc.Counter(
            "vmaf_scoring_requests_total_round2",
            "Round-2 test counter",
            ["endpoint", "status"],
            registry=registry,
        ),
        "scoring_errors_total": pc.Counter(
            "vmaf_scoring_errors_total_round2",
            "Round-2 test errors",
            registry=registry,
        ),
        "scoring_duration_seconds": pc.Histogram(
            "vmaf_scoring_duration_seconds_round2",
            "Round-2 test latency",
            buckets=[0.1, 1.0, 10.0, 60.0],
            registry=registry,
        ),
    }


@pytest_asyncio.fixture
async def round2_client(aiohttp_client: Any) -> TestClient:
    # VMAFX_MCP_HTTP_NO_AUTH must remain set for the full test duration;
    # the security middleware reads _no_auth_mode() at request time, not at
    # app-creation time.  Use a yield-fixture with patch.dict so the env var
    # stays alive while the test body makes HTTP requests.
    with patch.dict(os.environ, {"VMAFX_MCP_HTTP_NO_AUTH": "1"}, clear=False):
        metrics = _fresh_metrics_for_round2()
        app = ht._make_app(metrics)
        client = await aiohttp_client(app)
        yield client


@pytest.mark.asyncio
async def test_score_returns_400_when_width_missing(round2_client: TestClient) -> None:
    """The width/height/pixfmt/bitdepth check (lines 253-262) must fire 400
    when any of those keys is absent even if reference + distorted are given.
    """
    body = {
        "reference": "/tmp/r.yuv",
        "distorted": "/tmp/d.yuv",
        # width intentionally omitted
        "height": 1080,
        "pixfmt": "420",
        "bitdepth": 8,
    }
    resp = await round2_client.post("/v1/score", json=body)
    assert resp.status == 400
    payload = await resp.json()
    assert "missing required field" in payload["error"]
    assert "width" in payload["error"]


@pytest.mark.asyncio
async def test_score_returns_400_on_invalid_path(round2_client: TestClient, tmp_path: Path) -> None:
    """When ``_validate_path`` raises ``ValueError`` / ``FileNotFoundError``
    (lines 278-285), the endpoint must return 400 with the error surfaced.
    """
    body = {
        "reference": "/etc/passwd",  # outside allowlist
        "distorted": "/etc/passwd",
        "width": 1920,
        "height": 1080,
        "pixfmt": "420",
        "bitdepth": 8,
    }
    resp = await round2_client.post("/v1/score", json=body)
    assert resp.status == 400
    payload = await resp.json()
    assert "error" in payload
    assert "request_id" in payload


@pytest.mark.asyncio
async def test_score_returns_500_when_scorer_raises(
    round2_client: TestClient, tmp_path: Path
) -> None:
    """When ``_run_vmaf_score`` raises (lines 291-300) the endpoint must
    surface a 500 with the error string and bump the error counter.
    """
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    async def _boom(_req: Any) -> dict[str, Any]:
        raise RuntimeError("scorer exploded")

    with (
        patch.object(srv, "_validate_path", side_effect=lambda p: Path(p)),
        patch.object(srv, "_run_vmaf_score", new=AsyncMock(side_effect=_boom)),
    ):
        resp = await round2_client.post(
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
    assert resp.status == 500
    payload = await resp.json()
    assert "scorer exploded" in payload["error"]


# ===========================================================================
# server.py — _classify_source_resolution / _resolution_mismatch_warning
# ===========================================================================


def test_classify_source_resolution_4k_bucket() -> None:
    """The 4K branch (line 187) is hit only when the longer edge >= 3840."""
    assert srv._classify_source_resolution(3840, 2160) == "4k"
    assert srv._classify_source_resolution(7680, 4320) == "4k"


def test_classify_source_resolution_hd_bucket() -> None:
    assert srv._classify_source_resolution(1920, 1080) == "hd"
    assert srv._classify_source_resolution(1280, 720) == "hd"


def test_classify_source_resolution_sd_bucket() -> None:
    assert srv._classify_source_resolution(640, 480) == "sd"
    assert srv._classify_source_resolution(576, 324) == "sd"


def test_resolution_mismatch_warning_returns_none_for_unknown_model() -> None:
    """Unknown models (no entry in ``_MODEL_RES_HINTS``) must return None to
    avoid nagging on bespoke ONNX checkpoints (line 211-212).
    """
    assert srv._resolution_mismatch_warning("path=/custom/model.onnx", 1920, 1080) is None


def test_resolution_mismatch_warning_returns_none_when_classes_match() -> None:
    """An HD-trained model on an HD frame must NOT warn (line 215)."""
    assert srv._resolution_mismatch_warning("version=vmaf_v0.6.1", 1920, 1080) is None


def test_resolution_mismatch_warning_emits_hint_on_mismatch() -> None:
    """A 4K model on an SD frame must surface the hint (lines 216-221)."""
    msg = srv._resolution_mismatch_warning("version=vmaf_4k_v0.6.1", 576, 324)
    assert msg is not None
    assert "4k" in msg.lower()
    assert "sd" in msg.lower()
    assert "576x324" in msg


# ===========================================================================
# server.py — _describe_model_file JSON edge handling
# ===========================================================================


def test_describe_model_file_json_decode_error_is_swallowed(tmp_path: Path) -> None:
    """When the JSON model file is malformed (line 983-984), the metadata
    function must silently fall through and return the file-level fields
    (name/path/format/size_bytes) with model_type/feature_names = None.
    """
    bad = tmp_path / "broken.json"
    bad.write_text("{ this is not valid json")
    result = srv._describe_model_file(bad, tmp_path)
    assert result["name"] == "broken"
    assert result["format"] == "json"
    assert result["model_type"] is None
    assert result["feature_names"] is None


def test_describe_model_file_json_without_model_dict(tmp_path: Path) -> None:
    """When the JSON parses but has no ``model_dict`` key, the helper must
    return None for both model_type and feature_names rather than KeyErroring.
    """
    f = tmp_path / "nodict.json"
    f.write_text(json.dumps({"unrelated": "payload"}))
    result = srv._describe_model_file(f, tmp_path)
    assert result["model_type"] is None
    assert result["feature_names"] is None


# ===========================================================================
# server.py — _decode_to_yuv error paths
# ===========================================================================


def test_decode_to_yuv_raises_when_ffmpeg_missing(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Without ffmpeg on PATH (line 1672-1673) the helper must raise a
    RuntimeError with the install hint.
    """
    monkeypatch.setattr(srv.shutil, "which", lambda _name: None)

    async def _go() -> None:
        await srv._decode_to_yuv(tmp_path / "src.mp4", tmp_path / "dst.yuv", pix_fmt="yuv420p")

    with pytest.raises(RuntimeError, match="ffmpeg not on PATH"):
        asyncio.run(_go())


def test_decode_to_yuv_raises_on_ffmpeg_failure(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """When ffmpeg exits non-zero (lines 1691-1695) the helper must raise."""
    monkeypatch.setattr(srv.shutil, "which", lambda _name: "/usr/bin/ffmpeg")

    class _FakeProc:
        returncode = 1

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"", b"ffmpeg: synthetic failure"

    async def _fake_exec(*_args: Any, **_kwargs: Any) -> _FakeProc:
        return _FakeProc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)

    async def _go() -> None:
        await srv._decode_to_yuv(tmp_path / "src.mp4", tmp_path / "dst.yuv", pix_fmt="yuv420p")

    with pytest.raises(RuntimeError, match="ffmpeg decode failed"):
        asyncio.run(_go())


# ===========================================================================
# server.py — _ffprobe_geometry error paths
# ===========================================================================


def test_ffprobe_geometry_raises_when_ffprobe_missing(monkeypatch: pytest.MonkeyPatch) -> None:
    """Without ffprobe on PATH (line 1604-1605) the helper must raise."""
    monkeypatch.setattr(srv.shutil, "which", lambda _name: None)
    with pytest.raises(RuntimeError, match="ffprobe not on PATH"):
        srv._ffprobe_geometry(Path("/fake/video.mp4"))


def test_ffprobe_geometry_raises_on_nonzero_exit(monkeypatch: pytest.MonkeyPatch) -> None:
    """When ffprobe exits non-zero (lines 1625-1628), the helper must surface
    a RuntimeError with the trimmed stderr.
    """
    monkeypatch.setattr(srv.shutil, "which", lambda _name: "/usr/bin/ffprobe")

    class _R:
        returncode = 2
        stdout = ""
        stderr = "ffprobe: file not found"

    monkeypatch.setattr(srv.subprocess, "run", lambda *a, **k: _R())
    with pytest.raises(RuntimeError, match="ffprobe failed"):
        srv._ffprobe_geometry(Path("/fake/video.mp4"))


def test_ffprobe_geometry_raises_when_no_video_stream(monkeypatch: pytest.MonkeyPatch) -> None:
    """An audio-only container yields ``streams: []`` and must raise ValueError
    (lines 1631-1632).
    """
    monkeypatch.setattr(srv.shutil, "which", lambda _name: "/usr/bin/ffprobe")

    class _R:
        returncode = 0
        stdout = json.dumps({"streams": []})
        stderr = ""

    monkeypatch.setattr(srv.subprocess, "run", lambda *a, **k: _R())
    with pytest.raises(ValueError, match="no video stream"):
        srv._ffprobe_geometry(Path("/fake/audio.m4a"))


# ===========================================================================
# server.py — _run_vmaf_score error paths (Bug #1: explicit backend rejection)
# ===========================================================================


def test_run_vmaf_score_rejects_backend_not_advertised(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Bug #1 echo (lines 273-281): when the caller pins a backend the local
    binary does not advertise, the helper must raise rather than silently
    fall through to CPU.
    """
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"#!/bin/sh\nexit 0\n")
    fake_vmaf.chmod(0o755)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake_vmaf)
    monkeypatch.setattr(srv, "_probe_backends", lambda _: frozenset({"cpu"}))

    req = srv.ScoreRequest(
        ref=tmp_path / "r.yuv",
        dis=tmp_path / "d.yuv",
        width=64,
        height=64,
        pixfmt="420",
        bitdepth=8,
        backend="cuda",  # not advertised
    )

    with pytest.raises(RuntimeError, match="does not advertise"):
        asyncio.run(srv._run_vmaf_score(req))


def test_run_vmaf_score_raises_when_binary_missing(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Lines 266-268: missing binary must surface a clear build hint."""
    missing = tmp_path / "definitely-not-here" / "vmaf"
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: missing)
    req = srv.ScoreRequest(
        ref=tmp_path / "r.yuv",
        dis=tmp_path / "d.yuv",
        width=64,
        height=64,
        pixfmt="420",
        bitdepth=8,
    )
    with pytest.raises(RuntimeError, match="vmaf binary not found"):
        asyncio.run(srv._run_vmaf_score(req))


def test_run_vmaf_score_raises_on_nonzero_exit(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Lines 316-317: a non-zero vmaf exit must raise with the decoded stderr."""
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"#!/bin/sh\nexit 0\n")
    fake_vmaf.chmod(0o755)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake_vmaf)
    monkeypatch.setattr(srv, "_probe_backends", lambda _: frozenset({"cpu"}))

    class _Proc:
        returncode = 7

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"", b"vmaf synthetic failure"

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)
    req = srv.ScoreRequest(
        ref=tmp_path / "r.yuv",
        dis=tmp_path / "d.yuv",
        width=64,
        height=64,
        pixfmt="420",
        bitdepth=8,
        backend="auto",
    )
    with pytest.raises(RuntimeError, match="vmaf exited 7"):
        asyncio.run(srv._run_vmaf_score(req))


# ===========================================================================
# server.py — _run_vmaf_score_encoded integration via stubs
# ===========================================================================


def test_run_vmaf_score_encoded_decodes_then_scores(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """End-to-end stubbing of ``_run_vmaf_score_encoded`` (lines 1719-1750):
    geometry-probe + parallel decode + delegate to ``_run_vmaf_score`` and
    surface the encoded paths on the response payload.
    """
    monkeypatch.setattr(srv.shutil, "which", lambda _name: "/usr/bin/ffprobe")
    monkeypatch.setattr(srv, "_ffprobe_geometry", lambda _p: (1920, 1080, "420", 8))

    decode_calls: list[Path] = []

    async def _fake_decode(src: Path, dst: Path, *, pix_fmt: str) -> None:
        decode_calls.append(src)
        dst.write_bytes(b"\x00" * 16)

    async def _fake_score(req: Any) -> dict[str, Any]:
        return {"vmaf": 98.0, "frames": [], "pooled_metrics": {"vmaf": {"mean": 98.0}}}

    monkeypatch.setattr(srv, "_decode_to_yuv", _fake_decode)
    monkeypatch.setattr(srv, "_run_vmaf_score", _fake_score)

    ref = tmp_path / "ref.mp4"
    dis = tmp_path / "dis.mp4"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    result = asyncio.run(srv._run_vmaf_score_encoded(ref, dis))
    assert result["vmaf"] == 98.0
    assert result["reference_encoded"] == str(ref)
    assert result["distorted_encoded"] == str(dis)
    # Both inputs were decoded once.
    assert sorted(decode_calls) == sorted([ref, dis])


def test_run_vmaf_score_encoded_raises_when_ffprobe_missing(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Lines 1716-1717: missing ffprobe must raise even before geometry probe."""
    monkeypatch.setattr(srv.shutil, "which", lambda _name: None)
    ref = tmp_path / "r.mp4"
    dis = tmp_path / "d.mp4"

    async def _go() -> None:
        await srv._run_vmaf_score_encoded(ref, dis)

    with pytest.raises(RuntimeError, match="ffprobe not on PATH"):
        asyncio.run(_go())


# ===========================================================================
# server.py — _load_vlm import-fail path
# ===========================================================================


def test_load_vlm_returns_none_when_dependencies_absent(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Lines 542-546: when ``torch`` / ``transformers`` can't be imported the
    helper must return None (signalling "VLM unavailable" to callers).
    """
    # Reset the cached state so the import is exercised this call.
    monkeypatch.setitem(srv._vlm_state, "loaded", False)
    monkeypatch.setitem(srv._vlm_state, "pipeline", None)
    monkeypatch.setitem(srv._vlm_state, "model_id", None)

    real_import = __builtins__["__import__"] if isinstance(__builtins__, dict) else __import__

    def fake_import(name: str, *args: Any, **kwargs: Any) -> Any:
        if name in {"torch", "transformers"}:
            raise ImportError(f"simulated absence of {name}")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr("builtins.__import__", fake_import)
    assert srv._load_vlm() is None
    # Subsequent calls hit the cached-state short-circuit (line 538-539).
    assert srv._load_vlm() is None


# ===========================================================================
# server.py — _vmaf_version --version timeout fall-through
# ===========================================================================


@pytest.mark.asyncio
async def test_vmaf_version_handles_version_timeout(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Lines 1555-1556: when ``vmaf --version`` itself times out (or OSError)
    the helper must still return a payload with ``version=None`` and
    ``build_flags`` derived from ``--help``.

    ``_vmaf_version`` is an async coroutine; the test uses
    ``@pytest.mark.asyncio`` so that pytest-asyncio drives the event loop
    rather than calling the coroutine synchronously (which would return a
    coroutine object instead of the dict, causing the assertions to fail or
    raise ``AttributeError`` on Python 3.14+).
    """
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"#!/bin/sh\nexit 0\n")
    fake_vmaf.chmod(0o755)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake_vmaf)
    monkeypatch.setattr(srv, "_probe_backends", lambda _: frozenset({"cpu", "cuda"}))

    def _fake_run(argv: list[str], *_a: Any, **_k: Any) -> Any:
        # Simulate --version OSError.
        if any(a == "--version" for a in argv):
            raise OSError("synthetic OSError")
        raise AssertionError(f"unexpected argv: {argv}")

    monkeypatch.setattr(srv.subprocess, "run", _fake_run)
    result = await srv._vmaf_version()
    assert result["version"] is None
    assert result["build_flags"]["cuda"] is True
    assert result["build_flags"]["cpu"] is True
    assert result["error"] is None


# ===========================================================================
# server.py — _call_tool dispatch success-paths
# ===========================================================================


def test_call_tool_dispatch_list_backends_success() -> None:
    """The list_backends branch (line 2176) must return a JSON-encoded mapping
    with at least the cpu key. This exercises *successful* dispatch, distinct
    from PR #346's isError invariant suite which walks failure cases.
    """
    contents = asyncio.run(srv._call_tool("list_backends", {}))
    assert len(contents) == 1
    payload = json.loads(contents[0].text)
    assert "cpu" in payload


def test_call_tool_dispatch_list_models_success() -> None:
    """The list_models branch (line 2174) must return a wrapped list."""
    contents = asyncio.run(srv._call_tool("list_models", {}))
    payload = json.loads(contents[0].text)
    assert "models" in payload
    assert isinstance(payload["models"], list)


def test_call_tool_dispatch_list_extractors_success() -> None:
    """The list_extractors branch (line 2223) must return a wrapped list."""
    contents = asyncio.run(srv._call_tool("list_extractors", {}))
    payload = json.loads(contents[0].text)
    assert "extractors" in payload
    assert isinstance(payload["extractors"], list)


def test_call_tool_dispatch_vmaf_version_success(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """The vmaf_version branch (line 2211) must return the version dict."""
    missing = tmp_path / "no-such-vmaf"
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: missing)
    contents = asyncio.run(srv._call_tool("vmaf_version", {}))
    payload = json.loads(contents[0].text)
    assert "binary_path" in payload
    assert "build_flags" in payload


def test_call_tool_dispatch_describe_model_success() -> None:
    """The describe_model branch (line 2225) must surface the model metadata."""
    contents = asyncio.run(srv._call_tool("describe_model", {"name": "vmaf_v0.6.1"}))
    payload = json.loads(contents[0].text)
    assert payload["name"] == "vmaf_v0.6.1"
    assert payload["format"] == "json"


def test_call_tool_dispatch_probe_backend_not_compiled_in(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """The probe_backend branch (line 2209) must return a structured dict even
    when the backend is not compiled in. Exercises the dispatch row without
    requiring a real vmaf binary.
    """
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"")
    fake_vmaf.chmod(0o755)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake_vmaf)
    monkeypatch.setattr(srv, "_probe_backends", lambda _: frozenset({"cpu"}))

    contents = asyncio.run(srv._call_tool("probe_backend", {"backend": "cuda"}))
    payload = json.loads(contents[0].text)
    assert payload["backend"] == "cuda"
    assert payload["compiled_in"] is False
    assert payload["runtime_healthy"] is False


def test_call_tool_dispatch_compare_models_requires_non_empty_list() -> None:
    """The compare_models guard (lines 2188-2189) must raise ValueError on an
    empty list rather than crashing inside the eval helper.
    """

    async def _go() -> None:
        await srv._call_tool("compare_models", {"models": [], "features": "/tmp/x.parquet"})

    with pytest.raises(ValueError, match="non-empty list"):
        asyncio.run(_go())


# ===========================================================================
# server.py — _run_compare non-JSON stdout fallback
# ===========================================================================


def test_run_compare_falls_back_when_stdout_is_not_json(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Lines 1133-1139: when stdout is not valid JSON, ``_run_compare`` must
    return the raw stdout/stderr/exit_code triple rather than raise.
    """
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"#!/bin/sh\nexit 0\n")
    fake_tune.chmod(0o755)
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    class _Proc:
        returncode = 0

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"this is not JSON", b""

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)
    result = asyncio.run(srv._run_compare(src="/tmp/test.yuv"))
    assert result["exit_code"] == 0
    assert result["stdout"] == "this is not JSON"


# ===========================================================================
# server.py — _run_ladder + _run_tune_per_shot non-JSON fallback
# ===========================================================================


def test_run_ladder_falls_back_when_stdout_is_not_json(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Lines 1210-1212: ``_run_ladder`` with ``format='json'`` and non-JSON
    stdout must fall back to returning the raw manifest string.
    """
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"#!/bin/sh\nexit 0\n")
    fake_tune.chmod(0o755)
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    class _Proc:
        returncode = 0

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"not-json", b""

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)
    result = asyncio.run(
        srv._run_ladder(
            src="/tmp/x.yuv",
            resolutions="1920x1080",
            target_vmafs="95",
        )
    )
    assert result["manifest"] == "not-json"
    assert result["format"] == "json"


def test_run_tune_per_shot_falls_back_when_stdout_is_not_json(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Lines 1285-1287: tune-per-shot non-JSON stdout returns the triple."""
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"#!/bin/sh\nexit 0\n")
    fake_tune.chmod(0o755)
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    class _Proc:
        returncode = 0

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"plain-text", b""

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)
    result = asyncio.run(srv._run_tune_per_shot(src="/tmp/x.yuv"))
    assert result["exit_code"] == 0
    assert result["stdout"] == "plain-text"


# ===========================================================================
# server.py — _probe_backend JSON parse failure
# ===========================================================================


def test_probe_backend_json_parse_failure_reports_unhealthy(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Lines 1490-1498: when the vmaf JSON output is malformed (or absent), the
    probe must report runtime_healthy=False with a parse-error message rather
    than crashing.
    """
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"#!/bin/sh\nexit 0\n")
    fake_vmaf.chmod(0o755)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake_vmaf)
    monkeypatch.setattr(srv, "_probe_backends", lambda _: frozenset({"cpu"}))

    class _Proc:
        returncode = 0

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"", b""

    async def _fake_exec(*_a: Any, **kwargs: Any) -> _Proc:
        # Probe writes ref.yuv/dis.yuv; we intentionally do NOT write the
        # output JSON file so the read step raises -> covers the except.
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)
    result = asyncio.run(srv._probe_backend("cpu"))
    assert result["backend"] == "cpu"
    assert result["compiled_in"] is True
    assert result["runtime_healthy"] is False
    assert "parse" in result["error"].lower() or "no such file" in result["error"].lower()


# ===========================================================================
# server.py — main() backend selection
# ===========================================================================


def test_main_uses_asyncio_by_default(monkeypatch: pytest.MonkeyPatch) -> None:
    """Lines 2278-2285: ``main`` must call ``asyncio.run`` when
    ``VMAF_MCP_ASYNC`` is unset / equals 'asyncio'.

    We capture the coroutine arg and close it explicitly so we don't leave
    a "coroutine '_run' was never awaited" RuntimeWarning behind.
    """
    monkeypatch.delenv("VMAF_MCP_ASYNC", raising=False)

    captured: list[Any] = []

    def _capture_and_close(coro: Any, *_a: Any, **_k: Any) -> None:
        captured.append(coro)
        # Close the coroutine to suppress unawaited-coroutine warnings.
        try:
            coro.close()
        except Exception:
            pass

    monkeypatch.setattr(srv.asyncio, "run", _capture_and_close)
    srv.main()
    assert len(captured) == 1
    # The argument must be a coroutine (the _run() invocation).
    import inspect

    assert inspect.iscoroutine(captured[0])


def test_main_uses_anyio_when_env_set(monkeypatch: pytest.MonkeyPatch) -> None:
    """When ``VMAF_MCP_ASYNC=trio`` (or any non-asyncio value), ``main`` must
    dispatch to ``anyio.run`` with the named backend.
    """
    monkeypatch.setenv("VMAF_MCP_ASYNC", "trio")
    # Provide a stub anyio module so we don't require trio in CI. The stub's
    # ``run`` accepts the _run callable (NOT the coroutine), so no unawaited
    # coroutine is created here.
    stub = MagicMock()
    monkeypatch.setitem(sys.modules, "anyio", stub)
    srv.main()
    stub.run.assert_called_once()
    _args, kwargs = stub.run.call_args
    assert kwargs.get("backend") == "trio"


# ===========================================================================
# server.py — _run stdio loop entrypoint
# ===========================================================================


def test_run_invokes_stdio_server_and_server_run(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Lines 2272-2275: ``_run`` must enter ``stdio_server`` as an async
    context manager and call ``server.run`` with the resulting read/write
    streams + an initialization-options payload. We stub both so the test
    does not actually open stdin/stdout.
    """
    # Stub stdio_server to yield two sentinel streams.
    read_stream = MagicMock(name="read_stream")
    write_stream = MagicMock(name="write_stream")

    class _StdioCm:
        async def __aenter__(self) -> tuple[Any, Any]:
            return read_stream, write_stream

        async def __aexit__(self, *_a: Any) -> None:
            return None

    def _stdio_factory() -> _StdioCm:
        return _StdioCm()

    monkeypatch.setattr(srv, "stdio_server", _stdio_factory)

    # Pretend meson IS on PATH so the warning branch is not triggered
    # (we cover both states across tests).
    monkeypatch.setattr(srv.shutil, "which", lambda _n: "/usr/bin/meson")

    captured: dict[str, Any] = {}

    async def _fake_server_run(rs: Any, ws: Any, opts: Any) -> None:
        captured["rs"] = rs
        captured["ws"] = ws
        captured["opts"] = opts

    monkeypatch.setattr(srv.server, "run", _fake_server_run)
    asyncio.run(srv._run())
    assert captured["rs"] is read_stream
    assert captured["ws"] is write_stream
    assert captured["opts"] is not None


def test_run_warns_when_meson_missing(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """Lines 2272-2273: when ``meson`` is not on PATH, ``_run`` must emit a
    stderr warning before entering the stdio loop.
    """

    class _StdioCm:
        async def __aenter__(self) -> tuple[Any, Any]:
            return MagicMock(), MagicMock()

        async def __aexit__(self, *_a: Any) -> None:
            return None

    monkeypatch.setattr(srv, "stdio_server", lambda: _StdioCm())
    monkeypatch.setattr(srv.shutil, "which", lambda _n: None)

    async def _noop(*_a: Any, **_k: Any) -> None:
        return None

    monkeypatch.setattr(srv.server, "run", _noop)
    asyncio.run(srv._run())
    captured = capsys.readouterr()
    assert "meson not on PATH" in captured.err


# ===========================================================================
# server.py — _call_tool dispatch parametrised: progress-token extraction +
# delegation to vmaf_score, describe_worst_frames, vmaf_score_encoded, run_compare,
# run_ladder, run_tune_per_shot, run_benchmark
# ===========================================================================


def test_call_tool_dispatch_vmaf_score_success(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Line 2172: vmaf_score dispatch delegates to ``_run_vmaf_score`` and
    JSON-encodes the result. We stub the underlying scorer to avoid running
    the real binary.
    """
    ref = tmp_path / "r.yuv"
    dis = tmp_path / "d.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    async def _fake_score(_req: Any) -> dict[str, Any]:
        return {"vmaf": 99.0}

    monkeypatch.setattr(srv, "_validate_path", lambda p: Path(p))
    monkeypatch.setattr(srv, "_run_vmaf_score", _fake_score)

    contents = asyncio.run(
        srv._call_tool(
            "vmaf_score",
            {
                "ref": str(ref),
                "dis": str(dis),
                "width": 1920,
                "height": 1080,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )
    )
    payload = json.loads(contents[0].text)
    assert payload["vmaf"] == 99.0


def test_call_tool_dispatch_run_benchmark_success(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Line 2178: run_benchmark dispatch must await ``_run_benchmark`` and
    surface the resulting dict.
    """

    async def _fake_bench(progress_token: Any = None) -> dict[str, Any]:
        return {"exit_code": 0, "stdout": "ok", "stderr": ""}

    monkeypatch.setattr(srv, "_run_benchmark", _fake_bench)
    contents = asyncio.run(srv._call_tool("run_benchmark", {}))
    payload = json.loads(contents[0].text)
    assert payload["exit_code"] == 0


def test_call_tool_dispatch_describe_worst_frames_success(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Lines 2197-2207: describe_worst_frames dispatch must build the
    ScoreRequest and delegate to ``_describe_worst_frames``.
    """
    ref = tmp_path / "r.yuv"
    dis = tmp_path / "d.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    async def _fake_dwf(req: Any, *, n: int) -> dict[str, Any]:
        return {"model_id": None, "frames": [], "n_requested": n}

    monkeypatch.setattr(srv, "_validate_path", lambda p: Path(p))
    monkeypatch.setattr(srv, "_describe_worst_frames", _fake_dwf)

    contents = asyncio.run(
        srv._call_tool(
            "describe_worst_frames",
            {
                "ref": str(ref),
                "dis": str(dis),
                "width": 1920,
                "height": 1080,
                "pixfmt": "420",
                "bitdepth": 8,
                "n": 3,
            },
        )
    )
    payload = json.loads(contents[0].text)
    assert payload["n_requested"] == 3


def test_call_tool_dispatch_vmaf_score_encoded_success(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Line 2213: vmaf_score_encoded dispatch must delegate to
    ``_run_vmaf_score_encoded`` and JSON-encode the result.
    """
    ref = tmp_path / "r.mp4"
    dis = tmp_path / "d.mp4"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    async def _fake(*_a: Any, **_k: Any) -> dict[str, Any]:
        return {"vmaf": 88.0, "reference_encoded": str(ref)}

    monkeypatch.setattr(srv, "_validate_path", lambda p: Path(p))
    monkeypatch.setattr(srv, "_run_vmaf_score_encoded", _fake)

    contents = asyncio.run(
        srv._call_tool(
            "vmaf_score_encoded",
            {"reference_encoded": str(ref), "distorted_encoded": str(dis)},
        )
    )
    payload = json.loads(contents[0].text)
    assert payload["vmaf"] == 88.0


def test_call_tool_dispatch_run_compare_success(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Lines 2227-2239: run_compare dispatch must pass through the documented
    kwargs and surface the parsed report.
    """

    async def _fake(**kwargs: Any) -> dict[str, Any]:
        return {"reflect": dict(kwargs)}

    monkeypatch.setattr(srv, "_run_compare", _fake)
    contents = asyncio.run(
        srv._call_tool(
            "run_compare",
            {"src": "/tmp/x.yuv", "target_vmaf": 95.0, "width": 1920, "height": 1080},
        )
    )
    payload = json.loads(contents[0].text)
    assert payload["reflect"]["src"] == "/tmp/x.yuv"
    assert payload["reflect"]["target_vmaf"] == 95.0
    assert payload["reflect"]["width"] == 1920


def test_call_tool_dispatch_run_ladder_success(monkeypatch: pytest.MonkeyPatch) -> None:
    """Lines 2241-2251: run_ladder dispatch must pass src/resolutions/
    target_vmafs through.
    """

    async def _fake(**kwargs: Any) -> dict[str, Any]:
        return {"reflect": dict(kwargs)}

    monkeypatch.setattr(srv, "_run_ladder", _fake)
    contents = asyncio.run(
        srv._call_tool(
            "run_ladder",
            {
                "src": "/tmp/x.yuv",
                "resolutions": "1920x1080",
                "target_vmafs": "95,90",
            },
        )
    )
    payload = json.loads(contents[0].text)
    assert payload["reflect"]["resolutions"] == "1920x1080"


def test_call_tool_dispatch_run_tune_per_shot_success(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Lines 2253-2265: run_tune_per_shot dispatch must thread target_vmaf
    through.
    """

    async def _fake(**kwargs: Any) -> dict[str, Any]:
        return {"reflect": dict(kwargs)}

    monkeypatch.setattr(srv, "_run_tune_per_shot", _fake)
    contents = asyncio.run(
        srv._call_tool(
            "run_tune_per_shot",
            {"src": "/tmp/x.yuv", "target_vmaf": 93.5, "output": "/tmp/o.json"},
        )
    )
    payload = json.loads(contents[0].text)
    assert payload["reflect"]["target_vmaf"] == 93.5
    assert payload["reflect"]["output"] == "/tmp/o.json"
