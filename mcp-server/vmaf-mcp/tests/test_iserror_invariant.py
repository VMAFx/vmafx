# Copyright 2026 Lusoris
"""Comprehensive ``isError=True`` invariant + error-path tests for the MCP
server (`mcp-server/vmaf-mcp/src/vmaf_mcp/`).

Memory `project_mcp_iserror_must_be_true` (2026-05-19 audit) requires every
error path in an MCP server to either raise (so the mcp library sets
``isError=True`` on the ``CallToolResult``) or to explicitly construct an
``isError=True`` payload.  A silent ``isError=False`` return with an
``{"error": ...}`` body is a bug class — conformant clients
(mcp/client/session.py) then misclassify failures as successes.

The fork's ``_call_tool`` dispatcher (server.py L2140) was hardened in
ADR-0608 to never catch exceptions; raises propagate to the mcp library
which sets ``isError=True``.  This file pins that invariant by walking
every tool the dispatcher knows about and asserting the failure mode is
"raise", not "return error JSON".

Additional coverage targets (uncovered as of master ``bbcaa8d127``):

- ``_validate_path``                  — allowlist + file-existence errors
- ``_run_vmaf_score``                 — binary-missing + backend-not-advertised
- ``_decode_to_yuv`` /
  ``_extract_frame_png``              — ffmpeg-missing + pixfmt rejection
- ``_ffprobe_geometry``               — ffprobe-missing + no-stream
- ``_handle_metrics`` (HTTP)          — content-type regression
  (aiohttp rejects ``charset=`` in the ``content_type=`` kwarg)
- ``_resolve_port`` /
  ``_resolve_log_level`` /
  ``_apply_env_overrides``            — env-var precedence helpers
- ``main`` / ``_run``                 — entry-point dispatch
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Any
from unittest.mock import AsyncMock, MagicMock

import pytest

from vmaf_mcp import server as srv

# ---------------------------------------------------------------------------
# 1. isError invariant — every tool the dispatcher knows about raises on
#    malformed/unknown inputs.  Pins ADR-0608 / ADR-0613 / memory entry
#    `project_mcp_iserror_must_be_true`.
# ---------------------------------------------------------------------------


def test_call_tool_unknown_tool_raises_value_error() -> None:
    """``_call_tool`` MUST raise ``ValueError`` for unknown tool names.

    Returning a TextContent with ``isError`` implicitly False would cause
    conformant MCP clients to misclassify the error as a success.
    """

    async def run() -> Any:
        return await srv._call_tool("absolutely_no_such_tool", {})

    with pytest.raises(ValueError, match="unknown tool"):
        asyncio.run(run())


def test_call_tool_vmaf_score_missing_required_arg_raises() -> None:
    """``vmaf_score`` with no ``ref`` key must raise (KeyError or ValueError),
    NOT silently return ``{"error": ...}`` with ``isError=False``."""

    async def run() -> Any:
        return await srv._call_tool("vmaf_score", {})

    # KeyError on dict access is the natural propagation; ValueError would
    # come from ``_validate_path``.  Either is fine — silent return is not.
    with pytest.raises((KeyError, ValueError, FileNotFoundError)):
        asyncio.run(run())


def test_call_tool_vmaf_score_bad_path_raises() -> None:
    """Path that fails the allowlist guard must propagate as an exception."""

    async def run() -> Any:
        return await srv._call_tool(
            "vmaf_score",
            {
                "ref": "/tmp/_iserror_invariant_no_such_ref.yuv",
                "dis": "/tmp/_iserror_invariant_no_such_dis.yuv",
                "width": 320,
                "height": 240,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )

    with pytest.raises((ValueError, FileNotFoundError)):
        asyncio.run(run())


def test_call_tool_compare_models_empty_list_raises() -> None:
    """``compare_models`` with an empty ``models`` list must raise — not
    return ``{"ranked": [], "errors": []}`` with ``isError=False``."""

    async def run() -> Any:
        return await srv._call_tool(
            "compare_models",
            {"models": [], "features": "/tmp/nonexistent.parquet"},
        )

    with pytest.raises(ValueError, match="non-empty list"):
        asyncio.run(run())


def test_call_tool_compare_models_non_list_raises() -> None:
    """``compare_models`` with ``models`` not-a-list must raise."""

    async def run() -> Any:
        return await srv._call_tool(
            "compare_models",
            {"models": "not-a-list", "features": "/tmp/nonexistent.parquet"},
        )

    with pytest.raises(ValueError, match="non-empty list"):
        asyncio.run(run())


def test_call_tool_describe_model_unknown_raises() -> None:
    """``describe_model`` with an unknown name must raise — not return
    a fabricated ``{"format": null}`` payload."""

    async def run() -> Any:
        return await srv._call_tool(
            "describe_model",
            {"name": "_definitely_unknown_iserror_invariant_model_"},
        )

    with pytest.raises(ValueError, match="not found"):
        asyncio.run(run())


@pytest.mark.parametrize(
    "tool_name",
    [
        "vmaf_score",
        "describe_worst_frames",
        "vmaf_score_encoded",
        "eval_model_on_split",
        "compare_models",
        "describe_model",
        "run_compare",
        "run_ladder",
        "run_tune_per_shot",
    ],
)
def test_call_tool_missing_required_args_raises_on_every_tool(tool_name: str) -> None:
    """For every tool that requires arguments, calling with ``{}`` MUST
    raise an exception (KeyError on the dict access, ValueError from
    validation, or TypeError from the int(...) coercion). It MUST NOT
    silently return an ``isError=False`` payload.

    Tools without required arguments (``list_models``, ``list_backends``,
    ``run_benchmark``, ``list_extractors``, ``vmaf_version``,
    ``probe_backend``) are not in the parametrize — they have separate
    success-path tests in ``test_p1_tools.py`` /
    ``test_mcp_p0_adr0608.py``.
    """

    async def run() -> Any:
        return await srv._call_tool(tool_name, {})

    with pytest.raises((KeyError, ValueError, TypeError, FileNotFoundError)):
        asyncio.run(run())


# ---------------------------------------------------------------------------
# 2. _validate_path — allowlist + file-existence
# ---------------------------------------------------------------------------


def test_validate_path_outside_allowlist_raises(tmp_path: Path, monkeypatch) -> None:
    """A path that is not under any allowlisted root must raise ``ValueError``."""

    # Pin the allowlist to a temp dir we control so a passing path / failing
    # path comparison is unambiguous.
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    other = tmp_path.parent / "_iserror_invariant_outside"
    other.mkdir(exist_ok=True)
    try:
        rogue = other / "not_allowed.yuv"
        rogue.write_bytes(b"")
        # Clear the in-process cache so the env override actually takes
        # effect for ``_allowed_roots()``.
        # (No cache exists for _allowed_roots; the env is read on every call.)
        with pytest.raises(ValueError, match="not under an allowlisted root"):
            srv._validate_path(str(rogue))
    finally:
        for p in other.rglob("*"):
            if p.is_file():
                p.unlink(missing_ok=True)
        other.rmdir()


def test_validate_path_missing_file_raises(tmp_path: Path, monkeypatch) -> None:
    """An allowlisted path that does not exist must raise ``FileNotFoundError``."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    missing = tmp_path / "no_such_file.yuv"
    with pytest.raises(FileNotFoundError):
        srv._validate_path(str(missing))


def test_validate_path_accepts_allowlisted_file(tmp_path: Path, monkeypatch) -> None:
    """A real file under the allowlist must return the resolved Path."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    real = tmp_path / "real.yuv"
    real.write_bytes(b"\x00")
    resolved = srv._validate_path(str(real))
    assert resolved == real.resolve()


# ---------------------------------------------------------------------------
# 3. _run_vmaf_score — binary-missing + backend-not-advertised
# ---------------------------------------------------------------------------


def test_run_vmaf_score_binary_missing_raises(tmp_path: Path, monkeypatch) -> None:
    """When the vmaf binary cannot be found, ``_run_vmaf_score`` must raise
    ``RuntimeError`` so the MCP layer marks the call as ``isError=True``."""

    # Point the binary lookup at a path that does not exist.
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: tmp_path / "no_such_vmaf")
    req = srv.ScoreRequest(
        ref=tmp_path / "r.yuv",
        dis=tmp_path / "d.yuv",
        width=320,
        height=240,
        pixfmt="420",
        bitdepth=8,
    )

    with pytest.raises(RuntimeError, match="vmaf binary not found"):
        asyncio.run(srv._run_vmaf_score(req))


def test_run_vmaf_score_backend_not_advertised_raises(tmp_path: Path, monkeypatch) -> None:
    """Caller requests a backend the local binary did not compile in →
    refuse explicitly rather than silently fall back to CPU
    (regression bug #1 from the 2026-05-17 MCP probe)."""

    fake_bin = tmp_path / "vmaf"
    fake_bin.write_bytes(b"")
    fake_bin.chmod(0o755)

    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake_bin)
    # Only CPU is advertised — but the caller asks for CUDA.
    monkeypatch.setattr(srv, "_probe_backends", lambda _: frozenset({"cpu"}))

    req = srv.ScoreRequest(
        ref=tmp_path / "r.yuv",
        dis=tmp_path / "d.yuv",
        width=320,
        height=240,
        pixfmt="420",
        bitdepth=8,
        backend="cuda",
    )

    with pytest.raises(RuntimeError, match="does not advertise"):
        asyncio.run(srv._run_vmaf_score(req))


# ---------------------------------------------------------------------------
# 4. _extract_frame_png / _decode_to_yuv — ffmpeg-missing + pixfmt rejection
# ---------------------------------------------------------------------------


def test_extract_frame_png_ffmpeg_missing_raises(tmp_path: Path, monkeypatch) -> None:
    """``_extract_frame_png`` must raise ``RuntimeError`` when ffmpeg is
    absent from PATH — not silently produce a bogus PNG."""
    monkeypatch.setattr("shutil.which", lambda _cmd: None)
    yuv = tmp_path / "in.yuv"
    yuv.write_bytes(b"\x00" * 16)

    async def run() -> None:
        await srv._extract_frame_png(
            yuv,
            width=320,
            height=240,
            pixfmt="420",
            bitdepth=8,
            frame_index=0,
            out_png=tmp_path / "out.png",
        )

    with pytest.raises(RuntimeError, match="ffmpeg not on PATH"):
        asyncio.run(run())


def test_extract_frame_png_bad_pixfmt_raises(tmp_path: Path, monkeypatch) -> None:
    """An unsupported pixfmt/bitdepth combination must raise ``ValueError``
    before ffmpeg is invoked."""
    # ffmpeg appears to be on PATH so the function gets past the first guard.
    monkeypatch.setattr("shutil.which", lambda cmd: "/usr/bin/ffmpeg" if cmd == "ffmpeg" else None)
    yuv = tmp_path / "in.yuv"
    yuv.write_bytes(b"\x00" * 16)

    async def run() -> None:
        await srv._extract_frame_png(
            yuv,
            width=320,
            height=240,
            pixfmt="999",  # not in fmt_map
            bitdepth=42,
            frame_index=0,
            out_png=tmp_path / "out.png",
        )

    with pytest.raises(ValueError, match="unsupported pixfmt/bitdepth"):
        asyncio.run(run())


def test_decode_to_yuv_ffmpeg_missing_raises(tmp_path: Path, monkeypatch) -> None:
    """``_decode_to_yuv`` must raise ``RuntimeError`` when ffmpeg is absent."""
    monkeypatch.setattr("shutil.which", lambda _cmd: None)
    src = tmp_path / "encoded.mp4"
    src.write_bytes(b"\x00" * 16)

    async def run() -> None:
        await srv._decode_to_yuv(src, tmp_path / "out.yuv", pix_fmt="yuv420p")

    with pytest.raises(RuntimeError, match="ffmpeg not on PATH"):
        asyncio.run(run())


def test_decode_to_yuv_nonzero_exit_raises(tmp_path: Path, monkeypatch) -> None:
    """When ffmpeg exits non-zero, ``_decode_to_yuv`` must raise — not return
    a half-baked YUV file silently."""
    monkeypatch.setattr("shutil.which", lambda cmd: "/usr/bin/ffmpeg" if cmd == "ffmpeg" else None)

    async def fake_exec(*_args: Any, **_kwargs: Any) -> Any:
        proc = AsyncMock()
        proc.returncode = 1
        proc.communicate = AsyncMock(return_value=(b"", b"corrupt stream"))
        return proc

    monkeypatch.setattr(asyncio, "create_subprocess_exec", fake_exec)
    src = tmp_path / "encoded.mp4"
    src.write_bytes(b"\x00" * 16)

    async def run() -> None:
        await srv._decode_to_yuv(src, tmp_path / "out.yuv", pix_fmt="yuv420p")

    with pytest.raises(RuntimeError, match=r"ffmpeg decode failed \(rc=1\)"):
        asyncio.run(run())


# ---------------------------------------------------------------------------
# 5. _ffprobe_geometry — additional error paths
# ---------------------------------------------------------------------------


def test_ffprobe_geometry_ffprobe_missing_raises(monkeypatch) -> None:
    """When ffprobe is not on PATH, ``_ffprobe_geometry`` must raise
    ``RuntimeError``."""
    monkeypatch.setattr("shutil.which", lambda _cmd: None)
    with pytest.raises(RuntimeError, match="ffprobe not on PATH"):
        srv._ffprobe_geometry(Path("/tmp/anything.mp4"))


def test_ffprobe_geometry_no_video_stream_raises(monkeypatch) -> None:
    """An ffprobe payload with no video stream must raise ``ValueError``."""
    import shutil as _shutil
    import subprocess as _subprocess

    monkeypatch.setattr(
        _shutil, "which", lambda cmd: "/usr/bin/ffprobe" if cmd == "ffprobe" else None
    )

    def fake_run(*_args: Any, **_kwargs: Any) -> Any:
        r = MagicMock()
        r.returncode = 0
        r.stdout = json.dumps({"streams": []})
        r.stderr = ""
        return r

    monkeypatch.setattr(_subprocess, "run", fake_run)
    with pytest.raises(ValueError, match="no video stream"):
        srv._ffprobe_geometry(Path("/tmp/empty.mp4"))


def test_ffprobe_geometry_nonzero_exit_raises(monkeypatch) -> None:
    """When ffprobe exits non-zero, ``_ffprobe_geometry`` must raise."""
    import shutil as _shutil
    import subprocess as _subprocess

    monkeypatch.setattr(
        _shutil, "which", lambda cmd: "/usr/bin/ffprobe" if cmd == "ffprobe" else None
    )

    def fake_run(*_args: Any, **_kwargs: Any) -> Any:
        r = MagicMock()
        r.returncode = 1
        r.stdout = ""
        r.stderr = "moov atom not found"
        return r

    monkeypatch.setattr(_subprocess, "run", fake_run)
    with pytest.raises(RuntimeError, match="ffprobe failed"):
        srv._ffprobe_geometry(Path("/tmp/corrupt.mp4"))


# ---------------------------------------------------------------------------
# 6. _eval_model_on_split — argument-validation error paths
# ---------------------------------------------------------------------------


def test_eval_model_on_split_invalid_split_raises() -> None:
    """An unknown ``split`` name must raise ``ValueError`` before any I/O."""
    with pytest.raises(ValueError, match="split must be one of"):
        srv._eval_model_on_split(
            model=Path("/tmp/fake_model.onnx"),
            features=Path("/tmp/fake_features.parquet"),
            split="not_a_split",
            input_name="features",
        )


# ---------------------------------------------------------------------------
# 7. _compare_models — accumulates errors without raising
#    (this is the one error-shape that is INTENTIONAL; document it)
# ---------------------------------------------------------------------------


def test_compare_models_accumulates_per_model_errors(monkeypatch, tmp_path: Path) -> None:
    """``_compare_models`` runs every model in a loop and collects per-model
    failures in an ``errors`` list rather than raising on the first.  This
    is intentional batch semantics — callers asking "which of these N
    models works?" want the partial result.

    The dispatcher wraps the result in TextContent normally; the per-model
    error entries are surfaced to the user inside the success payload.
    """

    def fake_eval(model: Path, *_args: Any, **_kwargs: Any) -> dict[str, Any]:
        raise RuntimeError(f"synthetic failure for {model.name}")

    monkeypatch.setattr(srv, "_eval_model_on_split", fake_eval)

    result = srv._compare_models(
        models=[tmp_path / "a.onnx", tmp_path / "b.onnx"],
        features=tmp_path / "feat.parquet",
        split="test",
        input_name="features",
    )

    # Both models failed → ranked list is empty, errors list has 2 entries.
    assert result["ranked"] == []
    assert len(result["errors"]) == 2
    assert {e["model"] for e in result["errors"]} == {
        str(tmp_path / "a.onnx"),
        str(tmp_path / "b.onnx"),
    }
    assert all("synthetic failure" in e["error"] for e in result["errors"])


# ---------------------------------------------------------------------------
# 8. main() / _run dispatch — entry-point coverage
# ---------------------------------------------------------------------------


def test_main_default_asyncio_dispatch(monkeypatch) -> None:
    """``main()`` with no VMAF_MCP_ASYNC env var must dispatch via
    ``asyncio.run(_run())``."""
    monkeypatch.delenv("VMAF_MCP_ASYNC", raising=False)
    asyncio_run_calls: list[Any] = []

    def fake_asyncio_run(coro: Any) -> None:
        # Cancel the coroutine to avoid actually starting the MCP server.
        coro.close()
        asyncio_run_calls.append(coro)

    monkeypatch.setattr(srv.asyncio, "run", fake_asyncio_run)
    srv.main()
    assert len(asyncio_run_calls) == 1


def test_main_anyio_dispatch_routes_to_anyio_run(monkeypatch) -> None:
    """When ``VMAF_MCP_ASYNC`` is set to a non-asyncio backend name, ``main()``
    must route through ``anyio.run``.

    We don't have a trio install available in CI, so we patch the anyio
    import to a stub module and assert the call signature.
    """
    monkeypatch.setenv("VMAF_MCP_ASYNC", "trio")

    import sys
    import types

    fake_anyio = types.ModuleType("anyio")
    fake_anyio_calls: list[Any] = []

    def fake_anyio_run(func: Any, *, backend: str) -> None:
        fake_anyio_calls.append((func, backend))

    fake_anyio.run = fake_anyio_run  # type: ignore[attr-defined]
    monkeypatch.setitem(sys.modules, "anyio", fake_anyio)

    srv.main()
    assert len(fake_anyio_calls) == 1
    func, backend = fake_anyio_calls[0]
    assert backend == "trio"
    # ``func`` must be the module-level ``_run`` coroutine function.
    assert func is srv._run


# ---------------------------------------------------------------------------
# 9. HTTP transport — env-var precedence + content-type regression
#    (skipped when optional deps are missing)
# ---------------------------------------------------------------------------

_HT_DEPS_AVAILABLE = True
try:
    import aiohttp  # noqa: F401
    import prometheus_client  # noqa: F401
except ImportError:  # pragma: no cover — base install
    _HT_DEPS_AVAILABLE = False

ht_skip = pytest.mark.skipif(
    not _HT_DEPS_AVAILABLE,
    reason="aiohttp + prometheus_client not installed",
)


@ht_skip
def test_resolve_port_prefers_cli_over_env(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.setenv("VMAFX_PORT", "1234")
    assert ht._resolve_port(9876) == 9876


@ht_skip
def test_resolve_port_falls_back_to_env(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.setenv("VMAFX_PORT", "1234")
    assert ht._resolve_port(None) == 1234


@ht_skip
def test_resolve_port_invalid_env_falls_back_to_default(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.setenv("VMAFX_PORT", "not-a-number")
    assert ht._resolve_port(None) == 8080


@ht_skip
def test_resolve_port_unset_env_uses_default(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.delenv("VMAFX_PORT", raising=False)
    assert ht._resolve_port(None) == 8080


@ht_skip
def test_resolve_log_level_prefers_cli(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.setenv("VMAFX_LOG_LEVEL", "ERROR")
    assert ht._resolve_log_level("DEBUG") == "DEBUG"


@ht_skip
def test_resolve_log_level_falls_back_to_env(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.setenv("VMAFX_LOG_LEVEL", "WARNING")
    assert ht._resolve_log_level(None) == "WARNING"


@ht_skip
def test_resolve_log_level_default_info(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.delenv("VMAFX_LOG_LEVEL", raising=False)
    assert ht._resolve_log_level(None) == "INFO"


@ht_skip
def test_apply_env_overrides_propagates_vmaf_binary(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.delenv("VMAF_BIN", raising=False)
    monkeypatch.setenv("VMAFX_VMAF_BINARY", "/opt/vmaf/bin/vmaf")
    ht._apply_env_overrides()
    assert os_environ_get(monkeypatch, "VMAF_BIN") == "/opt/vmaf/bin/vmaf"


@ht_skip
def test_apply_env_overrides_does_not_clobber_existing_vmaf_bin(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.setenv("VMAF_BIN", "/already/set")
    monkeypatch.setenv("VMAFX_VMAF_BINARY", "/should/be/ignored")
    ht._apply_env_overrides()
    assert os_environ_get(monkeypatch, "VMAF_BIN") == "/already/set"


@ht_skip
def test_apply_env_overrides_appends_model_dir_to_allow(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.setenv("VMAF_MCP_ALLOW", "/root1:/root2")
    monkeypatch.setenv("VMAFX_MODEL_DIR", "/extra/models")
    ht._apply_env_overrides()
    new = os_environ_get(monkeypatch, "VMAF_MCP_ALLOW") or ""
    assert "/extra/models" in new.split(":")
    assert "/root1" in new.split(":")


@ht_skip
def test_apply_env_overrides_skips_duplicate_model_dir(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.setenv("VMAF_MCP_ALLOW", "/root1:/extra/models")
    monkeypatch.setenv("VMAFX_MODEL_DIR", "/extra/models")
    ht._apply_env_overrides()
    new = os_environ_get(monkeypatch, "VMAF_MCP_ALLOW") or ""
    # Already present — must not be appended a second time.
    assert new.count("/extra/models") == 1


@ht_skip
def test_apply_env_overrides_sets_allow_when_empty(monkeypatch) -> None:
    from vmaf_mcp import http_transport as ht

    monkeypatch.delenv("VMAF_MCP_ALLOW", raising=False)
    monkeypatch.setenv("VMAFX_MODEL_DIR", "/only/models")
    ht._apply_env_overrides()
    assert os_environ_get(monkeypatch, "VMAF_MCP_ALLOW") == "/only/models"


def os_environ_get(monkeypatch, key: str) -> str | None:
    """Helper: read os.environ in the same scope monkeypatch patched."""
    import os

    return os.environ.get(key)


# ---------------------------------------------------------------------------
# 10. HTTP /metrics regression — aiohttp.Response rejects ``charset=`` in
#     the ``content_type=`` kwarg (raised before the fix in this PR).
# ---------------------------------------------------------------------------


@ht_skip
def test_handle_metrics_does_not_raise_on_charset_content_type() -> None:
    """``_handle_metrics`` MUST NOT pass a ``Content-Type`` value containing
    ``charset=`` via the ``content_type=`` kwarg of ``aiohttp.web.Response``.

    Prior to the fix in this PR, ``prometheus_client.CONTENT_TYPE_LATEST``
    (``text/plain; version=0.0.4; charset=utf-8``) was passed as
    ``content_type=``, which raised ``ValueError("charset must not be in
    content_type argument")`` at response construction.  The /metrics
    endpoint then returned ``500`` instead of the Prometheus scrape body.

    This test exercises ``_handle_metrics`` directly (no aiohttp_client) so
    it is fast and does not require pytest-aiohttp.
    """
    from vmaf_mcp import http_transport as ht

    async def run() -> Any:
        # ``request`` is unused inside ``_handle_metrics``; a sentinel is OK.
        return await ht._handle_metrics(MagicMock())

    response = asyncio.run(run())
    assert response.status == 200
    # The aiohttp Response should expose the prometheus content-type via
    # the explicit header, not via the ``content_type=`` kwarg.
    assert "text/plain" in response.headers["Content-Type"]
