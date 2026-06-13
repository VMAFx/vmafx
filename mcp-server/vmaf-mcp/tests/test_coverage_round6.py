# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Round-6 coverage uplift for vmaf-mcp.

Closes residual gaps after rounds 1-5. All tests are self-contained —
no network, no vmaf binary, no GPU.

Targeted lines (server.py baseline 91% = 77 missing):
- 128-137: asyncio.CancelledError handler in _communicate_with_timeout
- 323: null-bytes guard in _validate_path
- 329-336: URL scheme check (file:// strip) in _validate_media_path
- 341: allowlist raise in _validate_media_path
- 450-452: lazy lock creation in _get_backend_probe_lock
- 566: double-check cache inside per-key lock in _probe_backends_async
- 632: subsample branch in _run_vmaf_score
- 651-652: JSONDecodeError raise in _run_vmaf_score
- 980: ffmpeg frame-extract non-zero rc raise
- 1240: allowlist raise in _describe_model (direct-path branch)
- 1367-1371: Exception catch in _send_progress
- 1422: elif target_vmaf branch in _run_compare
- 1980-1981: ffprobe JSONDecodeError raise
- 2088: unsupported pixfmt/bitdepth raise in _run_vmaf_score_encoded
- 2106: BaseException re-raise in _run_vmaf_score_encoded decode loop
- 2528-2529: progress token from request context meta in _call_tool
- 2699: depth-check raise in _check_depth
- 2739-2773: _emit_parse_error full body (JSON array, parse error, id extraction)
- 2805-2816, 2819, 2822-2834: _ParseErrorFilteredStdin iteration machinery
- 2860-2862: explicit anyio backend name branch in main()
- 2866: __main__ entry point

ADR-0108 deliverables:
- Research digest: no digest needed: coverage gap-fill.
- Decision matrix: no alternatives: only-one-way fix.
- AGENTS.md invariant note: no rebase-sensitive invariants.
- Reproducer:
    cd mcp-server/vmaf-mcp && python -m pytest tests/test_coverage_round6.py -v
- Changelog fragment: changelog.d/added/mcp-server-coverage-round6.md.
- Rebase note: no rebase impact (fork-local Python tests only).
"""

from __future__ import annotations

import asyncio
import io
import json
import os
import sys
from pathlib import Path
from typing import Any
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from vmaf_mcp import server as srv

# ---------------------------------------------------------------------------
# _communicate_with_timeout — CancelledError handler (lines 128-137)
# ---------------------------------------------------------------------------


def test_communicate_with_timeout_cancelled_kills_and_reraises() -> None:
    """When the coroutine is cancelled, the child process is killed and
    CancelledError is re-raised so asyncio can propagate the cancellation."""

    class _FakeProc:
        killed = False
        returncode = None

        def kill(self) -> None:
            self.killed = True

        async def communicate(self) -> tuple[bytes, bytes]:
            # First call hangs (simulates what wait_for would cancel),
            # second call (drain) returns immediately.
            return b"", b""

    proc = _FakeProc()

    async def _run() -> None:
        # Patch wait_for so the first call raises CancelledError.
        call_count = {"n": 0}
        original_wait_for = asyncio.wait_for

        async def _fake_wait_for(coro: Any, timeout: float) -> Any:
            call_count["n"] += 1
            if call_count["n"] == 1:
                # Close the coroutine to avoid "was never awaited" warning.
                coro.close()
                raise asyncio.CancelledError()
            return await original_wait_for(coro, timeout=timeout)

        with (
            patch.object(asyncio, "wait_for", side_effect=_fake_wait_for),
            pytest.raises(asyncio.CancelledError),
        ):
            await srv._communicate_with_timeout(proc, timeout=5.0)  # type: ignore[arg-type]
        assert proc.killed, "process was not killed after CancelledError"

    asyncio.run(_run())


# ---------------------------------------------------------------------------
# _validate_media_path — null-bytes guard (line 323)
# ---------------------------------------------------------------------------


def test_validate_media_path_rejects_null_bytes() -> None:
    """_validate_media_path must raise ValueError when the path contains null bytes."""
    with pytest.raises(ValueError, match="null bytes"):
        srv._validate_media_path("/some/path\x00evil")


# ---------------------------------------------------------------------------
# _validate_media_path — URL scheme + allowlist (lines 329-336, 341)
# ---------------------------------------------------------------------------


def test_validate_media_path_strips_file_scheme(tmp_path: Path) -> None:
    """_validate_media_path must accept file:// paths and strip the scheme."""
    yuv = tmp_path / "clip.yuv"
    yuv.write_bytes(b"\x00" * 8)
    # Allow tmp_path so the resolved path passes the allowlist check.
    with patch.dict(os.environ, {"VMAF_MCP_ALLOW": str(tmp_path)}, clear=False):
        result = srv._validate_media_path(f"file://{yuv}")
    assert result == str(yuv.resolve())


def test_validate_media_path_rejects_disallowed_scheme() -> None:
    """_validate_media_path must reject non-file URL schemes (e.g. http://)."""
    with pytest.raises(ValueError, match=r"scheme.*not permitted"):
        srv._validate_media_path("http://example.com/video.yuv")


def test_validate_media_path_rejects_path_outside_allowlist() -> None:
    """_validate_media_path must raise ValueError when path is not allowlisted."""
    with pytest.raises(ValueError, match="not under an allowlisted root"):
        # /proc is never in the default allowlist and cannot be added via env
        # without explicitly setting it; pick a path guaranteed to be absent.
        srv._validate_media_path("/proc/version")


# ---------------------------------------------------------------------------
# _get_backend_probe_lock — lazy lock creation (lines 450-452)
# ---------------------------------------------------------------------------


def test_get_backend_probe_lock_creates_lazily() -> None:
    """_get_backend_probe_lock must create the Lock on first call."""
    original = srv._BACKEND_PROBE_LOCK
    srv._BACKEND_PROBE_LOCK = None  # force lazy path

    async def _check() -> None:
        lock = srv._get_backend_probe_lock()
        assert isinstance(lock, asyncio.Lock)
        # Second call must return the same object.
        assert srv._get_backend_probe_lock() is lock

    try:
        asyncio.run(_check())
    finally:
        srv._BACKEND_PROBE_LOCK = original


# ---------------------------------------------------------------------------
# _probe_backends_async — double-check cache after lock (line 566)
# ---------------------------------------------------------------------------


def test_probe_backends_async_double_check_cache(tmp_path: Path) -> None:
    """When a sibling coroutine populates the cache while a second coroutine
    waits for the per-key lock, the second coroutine must use the inner
    double-check (line 566) and return the cached result without spawning
    a subprocess.

    Technique: fire two concurrent coroutines. The first acquires the lock,
    populates the cache via a slow (one-tick) stub, then releases. The second
    coroutine hits the fast-path MISS (cache is empty), waits for the lock,
    then uses the inner check and returns the cached result.
    """
    fake = tmp_path / "vmaf_dc2"
    fake.write_text("#!/bin/sh\nexit 0\n")
    fake.chmod(0o755)
    key = str(fake)

    srv._BACKEND_PROBE_CACHE.pop(key, None)
    srv._BACKEND_PROBE_LOCKS.pop(key, None)
    srv._BACKEND_PROBE_LOCK_DICT_LOCK = None

    call_count = {"n": 0}
    expected = frozenset({"cpu", "cuda"})

    def _counting_run(*_a: Any, **_k: Any) -> Any:
        call_count["n"] += 1

        class _R:
            stdout = "--no_cuda    disable CUDA backend\n"
            stderr = ""
            returncode = 0

        return _R()

    async def _run_two() -> list[frozenset]:
        # Patch subprocess.run for the thread-based probe.
        with patch.object(srv.subprocess, "run", _counting_run):
            # Both coroutines start concurrently; asyncio schedules them.
            results = await asyncio.gather(
                srv._probe_backends_async(fake),
                srv._probe_backends_async(fake),
            )
        return list(results)

    results = asyncio.run(_run_two())

    # subprocess must have been called exactly once (the winner runs the probe).
    assert call_count["n"] == 1, f"subprocess called {call_count['n']} times; expected 1"
    assert all(r == expected for r in results), f"inconsistent results: {results}"
    # Clean up.
    srv._BACKEND_PROBE_CACHE.pop(key, None)
    srv._BACKEND_PROBE_LOCKS.pop(key, None)


# ---------------------------------------------------------------------------
# _run_vmaf_score — subsample and JSONDecodeError (lines 632, 651-652)
# ---------------------------------------------------------------------------


def test_run_vmaf_score_subsample_arg_added(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When ScoreRequest.subsample > 1 the --subsample flag must appear in argv."""
    captured: list[list[str]] = []
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake_vmaf)
    monkeypatch.setattr(srv, "_probe_backends_async", AsyncMock(return_value=frozenset({"cpu"})))

    output_json = {"pooled_metrics": {"vmaf": {"mean": 90.0}}, "frames": []}

    async def _fake_exec(*argv: str, **_kw: Any) -> Any:
        captured.append(list(argv))
        # Write a minimal output JSON to the --output file.
        for i, a in enumerate(argv):
            if a == "-o" and i + 1 < len(argv):
                Path(argv[i + 1]).write_text(json.dumps(output_json), encoding="utf-8")
        proc = AsyncMock()
        proc.returncode = 0
        proc.communicate = AsyncMock(return_value=(b"", b""))
        return proc

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)

    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    req = srv.ScoreRequest(
        ref=ref,
        dis=dis,
        width=16,
        height=1,
        pixfmt="420",
        bitdepth=8,
        model="version=vmaf_v0.6.1",
        backend="auto",
        subsample=4,
    )
    asyncio.run(srv._run_vmaf_score(req))

    flat = captured[0]
    assert "--subsample" in flat
    idx = flat.index("--subsample")
    assert flat[idx + 1] == "4"


def test_run_vmaf_score_json_decode_error_raises(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When the vmaf output file contains malformed JSON, RuntimeError is raised."""
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake_vmaf)
    monkeypatch.setattr(srv, "_probe_backends_async", AsyncMock(return_value=frozenset({"cpu"})))

    async def _fake_exec(*argv: str, **_kw: Any) -> Any:
        for i, a in enumerate(argv):
            if a == "-o" and i + 1 < len(argv):
                Path(argv[i + 1]).write_text("NOT JSON", encoding="utf-8")
        proc = AsyncMock()
        proc.returncode = 0
        proc.communicate = AsyncMock(return_value=(b"", b""))
        return proc

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)

    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 8)
    dis.write_bytes(b"\x00" * 8)

    req = srv.ScoreRequest(
        ref=ref,
        dis=dis,
        width=8,
        height=1,
        pixfmt="420",
        bitdepth=8,
        model="version=vmaf_v0.6.1",
        backend="auto",
    )
    with pytest.raises(RuntimeError, match="vmaf output unreadable"):
        asyncio.run(srv._run_vmaf_score(req))


# ---------------------------------------------------------------------------
# _extract_frame_png — nonzero rc raise (line 980)
# ---------------------------------------------------------------------------


def test_extract_frame_png_nonzero_rc_raises(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When ffmpeg exits non-zero, _extract_frame_png must raise RuntimeError."""

    async def _fake_exec(*_a: Any, **_kw: Any) -> Any:
        proc = AsyncMock()
        proc.returncode = 1
        proc.communicate = AsyncMock(return_value=(b"", b"frame-extract error"))
        return proc

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)
    # Bypass the shutil.which("ffmpeg") guard.
    monkeypatch.setattr(srv.shutil, "which", lambda _: "/usr/bin/ffmpeg")
    yuv = tmp_path / "clip.yuv"
    yuv.write_bytes(b"")
    out_png = tmp_path / "frame.png"

    with pytest.raises(RuntimeError, match="ffmpeg frame-extract failed"):
        asyncio.run(
            srv._extract_frame_png(
                yuv,
                width=16,
                height=8,
                pixfmt="420",
                bitdepth=8,
                frame_index=0,
                out_png=out_png,
            )
        )


# ---------------------------------------------------------------------------
# _describe_model — allowlist raise (line 1240)
# ---------------------------------------------------------------------------


def test_describe_model_direct_path_outside_allowlist_raises(tmp_path: Path) -> None:
    """When step-1 resolves an exact path that is not under an allowlisted root,
    ValueError is raised (covers the allowlist check on the direct-path branch).
    """
    model_file = tmp_path / "my_model.json"
    model_file.write_text(json.dumps({"model": {"model_type": "VMAF"}}), encoding="utf-8")

    # Ensure tmp_path is NOT in VMAF_MCP_ALLOW (it is not by default).
    with (
        patch.dict(os.environ, {"VMAF_MCP_ALLOW": ""}, clear=False),
        pytest.raises(ValueError, match="not under an allowlisted root"),
    ):
        srv._describe_model(str(model_file))


# ---------------------------------------------------------------------------
# _send_progress — generic Exception swallowed (lines 1367-1371)
# ---------------------------------------------------------------------------


def test_send_progress_swallows_unexpected_exception(monkeypatch: pytest.MonkeyPatch) -> None:
    """When send_progress_notification raises an unexpected exception it must
    be swallowed and logged as a warning — never propagated.
    """
    import mcp.server

    class _BoomContext:
        class request:
            class params:
                class meta:
                    progressToken = "tok"

    # Make the request context return a value so we reach send_progress_notification.
    monkeypatch.setattr(mcp.server.Server, "request_context", _BoomContext, raising=False)

    async def _boom(*_a: Any, **_kw: Any) -> None:
        raise OSError("unexpected transport failure")

    monkeypatch.setattr(srv.server, "send_progress_notification", _boom, raising=False)

    # Must complete without raising.
    asyncio.run(srv._send_progress("tok", 0.5, 1.0, "test"))


# ---------------------------------------------------------------------------
# _run_compare — elif target_vmaf branch (line 1422)
# ---------------------------------------------------------------------------


def test_run_compare_target_vmaf_single_value(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """When only target_vmaf (not target_vmafs) is passed, --target-vmaf appears."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", "/tmp")
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    captured: list[list[str]] = []

    class _Proc:
        returncode = 0

        async def communicate(self) -> tuple[bytes, bytes]:
            return json.dumps({"ok": True}).encode(), b""

    async def _fake_exec(*argv: str, **_kw: Any) -> _Proc:
        captured.append(list(argv))
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)
    asyncio.run(srv._run_compare(src="/tmp/x.yuv", target_vmaf=92.0))
    argv = captured[0]
    assert "--target-vmaf" in argv
    idx = argv.index("--target-vmaf")
    assert argv[idx + 1] == "92.0"
    assert "--target-vmafs" not in argv


# ---------------------------------------------------------------------------
# _ffprobe_geometry — JSONDecodeError raise (lines 1980-1981)
# ---------------------------------------------------------------------------


def test_ffprobe_geometry_json_decode_error_raises(monkeypatch: pytest.MonkeyPatch) -> None:
    """When ffprobe stdout is not valid JSON, RuntimeError is raised."""

    class _Result:
        returncode = 0
        stdout = "NOT JSON"
        stderr = ""

    monkeypatch.setattr(srv.subprocess, "run", lambda *_a, **_kw: _Result())
    with pytest.raises(RuntimeError, match="ffprobe output malformed"):
        srv._ffprobe_geometry(Path("/some/video.mp4"))


# ---------------------------------------------------------------------------
# _run_vmaf_score_encoded — unsupported pixfmt/bitdepth (line 2088)
# ---------------------------------------------------------------------------


def test_run_vmaf_score_encoded_unsupported_pixfmt_raises(tmp_path: Path) -> None:
    """When ffprobe returns an unsupported pixfmt/bitdepth combo, ValueError is raised."""
    ref = tmp_path / "ref.mp4"
    dis = tmp_path / "dis.mp4"
    ref.write_bytes(b"")
    dis.write_bytes(b"")

    # ffprobe_geometry_async returns (width, height, pixfmt, bitdepth).
    # Use a pixfmt/bitdepth combo absent from _PIXFMT_TO_FFMPEG.
    with (
        patch.object(
            srv,
            "_ffprobe_geometry_async",
            AsyncMock(return_value=(1920, 1080, "411", 8)),
        ),
        pytest.raises(ValueError, match="unsupported pixfmt/bitdepth"),
    ):
        asyncio.run(
            srv._run_vmaf_score_encoded(
                ref_path=ref,
                dis_path=dis,
            )
        )


# ---------------------------------------------------------------------------
# _run_vmaf_score_encoded — decode exception re-raised (line 2106)
# ---------------------------------------------------------------------------


def test_run_vmaf_score_encoded_decode_error_reraises(tmp_path: Path) -> None:
    """When _decode_to_yuv raises, the exception is re-raised from the gather."""
    ref = tmp_path / "ref.mp4"
    dis = tmp_path / "dis.mp4"
    ref.write_bytes(b"")
    dis.write_bytes(b"")

    with (
        patch.object(
            srv,
            "_ffprobe_geometry_async",
            AsyncMock(return_value=(1920, 1080, "420", 8)),
        ),
        patch.object(
            srv,
            "_decode_to_yuv",
            AsyncMock(side_effect=RuntimeError("ffmpeg decode exploded")),
        ),
        pytest.raises(RuntimeError, match="ffmpeg decode exploded"),
    ):
        asyncio.run(
            srv._run_vmaf_score_encoded(
                ref_path=ref,
                dis_path=dis,
            )
        )


# ---------------------------------------------------------------------------
# _check_depth — depth exceeded raise (line 2699)
# ---------------------------------------------------------------------------


def test_check_depth_raises_on_deep_nesting() -> None:
    """_check_depth must raise ValueError when nesting exceeds max_depth."""
    # Build a dict nested 55 levels deep (default max is 50).
    deep: dict[str, Any] = {}
    node = deep
    for _ in range(55):
        child: dict[str, Any] = {}
        node["x"] = child
        node = child
    with pytest.raises(ValueError, match="nesting exceeds maximum depth"):
        srv._check_depth(deep)


def test_check_depth_accepts_shallow() -> None:
    """_check_depth must not raise for a shallowly-nested dict."""
    srv._check_depth({"a": {"b": {"c": 1}}})


# ---------------------------------------------------------------------------
# _emit_parse_error — full body: JSON array, parse error, id extraction (2739-2773)
# ---------------------------------------------------------------------------


def test_emit_parse_error_on_invalid_json(capsys: pytest.CaptureFixture) -> None:
    """_emit_parse_error must write a JSON-RPC parse-error response to stdout
    when raw_line is not valid JSON.
    """
    srv._emit_parse_error("NOT JSON AT ALL", ValueError("bad json"))
    captured = capsys.readouterr()
    payload = json.loads(captured.out.strip())
    assert payload["error"]["code"] == -32700
    assert payload["id"] is None


def test_emit_parse_error_on_json_array(capsys: pytest.CaptureFixture) -> None:
    """_emit_parse_error must emit -32600 (Invalid Request) when raw_line is
    a valid JSON array (JSON-RPC batch — not supported).
    """
    srv._emit_parse_error(json.dumps([{"jsonrpc": "2.0"}]), ValueError("batch"))
    captured = capsys.readouterr()
    payload = json.loads(captured.out.strip())
    assert payload["error"]["code"] == -32600
    assert payload["error"]["message"] == "Invalid Request"


def test_emit_parse_error_extracts_id_from_dict(capsys: pytest.CaptureFixture) -> None:
    """When raw_line is a JSON object with a parseable id field, _emit_parse_error
    must include that id in the response (spec §5.1 — when id is determinable).
    """
    srv._emit_parse_error(json.dumps({"id": 42, "method": "broken"}), ValueError("oops"))
    captured = capsys.readouterr()
    payload = json.loads(captured.out.strip())
    assert payload["id"] == 42


# ---------------------------------------------------------------------------
# _ParseErrorFilteredStdin — aiter / anext / _make_inner (2805-2834)
# ---------------------------------------------------------------------------


def test_parse_error_filtered_stdin_make_inner_no_buffer(monkeypatch: pytest.MonkeyPatch) -> None:
    """When sys.stdin has no .buffer attribute, _make_inner returns sys.stdin directly."""
    # Remove buffer attribute from stdin.
    fake_stdin = io.StringIO("line1\nline2\n")
    monkeypatch.setattr(sys, "stdin", fake_stdin)

    wrapper = srv._ParseErrorFilteredStdin()
    inner = wrapper._make_inner()
    # Without buffer, we get sys.stdin back (the StringIO itself).
    assert inner is fake_stdin


def test_parse_error_filtered_stdin_yields_valid_lines() -> None:
    """_ParseErrorFilteredStdin must yield lines that pass JSON-RPC validation
    and silently drop invalid lines, emitting a parse-error response instead.
    """

    valid_msg = json.dumps(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "test", "version": "0"},
            },
        }
    )
    invalid_msg = "this is not json-rpc at all"

    lines_to_yield = [valid_msg, invalid_msg]

    class _FakeInner:
        def __aiter__(self) -> "_FakeInner":
            return self

        async def __anext__(self) -> str:
            if not lines_to_yield:
                raise StopAsyncIteration
            return lines_to_yield.pop(0)

    wrapper = srv._ParseErrorFilteredStdin()
    wrapper._inner = _FakeInner()

    collected: list[str] = []

    async def _collect() -> None:
        assert wrapper.__aiter__() is wrapper  # covers line 2819
        async for line in wrapper:
            collected.append(line)

    with patch.object(srv, "_emit_parse_error"):
        asyncio.run(_collect())

    # Only the valid JSON-RPC line should be yielded.
    assert len(collected) == 1
    assert json.loads(collected[0])["id"] == 1


# ---------------------------------------------------------------------------
# main() — explicit anyio backend name branch (lines 2860-2862)
# ---------------------------------------------------------------------------


def test_main_explicit_anyio_backend(monkeypatch: pytest.MonkeyPatch) -> None:
    """When VMAF_MCP_ASYNC is an explicit backend name (not asyncio/trio),
    anyio.run must be called with that backend name.
    """
    monkeypatch.setenv("VMAF_MCP_ASYNC", "asyncio_custom")

    calls: list[dict[str, Any]] = []

    def _fake_anyio_run(coro: Any, backend: str = "asyncio") -> None:
        calls.append({"backend": backend})

    with patch.dict("sys.modules", {"anyio": MagicMock(run=_fake_anyio_run)}):

        # Re-import to pick up the patched anyio.
        anyio_mod = MagicMock()
        anyio_mod.run = _fake_anyio_run
        with patch("vmaf_mcp.server.anyio", anyio_mod, create=True):
            srv.main()

    assert any(c["backend"] == "asyncio_custom" for c in calls)


# ---------------------------------------------------------------------------
# __main__ entry point (line 2866)
# ---------------------------------------------------------------------------


def test_main_callable() -> None:
    """main() must be callable without error when _run is patched out."""
    with (
        patch.dict(os.environ, {"VMAF_MCP_ASYNC": ""}, clear=False),
        patch.object(srv, "_run", AsyncMock()),
    ):
        srv.main()


# ---------------------------------------------------------------------------
# _ParseErrorFilteredStdin — _make_inner WITH buffer (line 2813)
# and __anext__ lazy-inner-init path (line 2825)
# ---------------------------------------------------------------------------


def test_parse_error_filtered_stdin_make_inner_with_buffer(monkeypatch: pytest.MonkeyPatch) -> None:
    """When sys.stdin has a .buffer attribute, _make_inner wraps it with anyio."""
    import io as _io

    fake_buffer = _io.BytesIO(b"hello\n")
    fake_stdin = _io.TextIOWrapper(fake_buffer, encoding="utf-8")
    monkeypatch.setattr(sys, "stdin", fake_stdin)

    wrapper = srv._ParseErrorFilteredStdin()
    # anyio.wrap_file is called for the binary buffer; just verify no exception.
    inner = wrapper._make_inner()
    assert inner is not None


def test_parse_error_filtered_stdin_lazy_inner_init() -> None:
    """When _inner is None at __anext__ call time, it must be lazily initialised."""
    lines_to_yield = [
        json.dumps(
            {
                "jsonrpc": "2.0",
                "id": 99,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {},
                    "clientInfo": {"name": "t", "version": "0"},
                },
            }
        )
    ]

    class _FakeInner:
        def __aiter__(self) -> "_FakeInner":
            return self

        async def __anext__(self) -> str:
            if not lines_to_yield:
                raise StopAsyncIteration
            return lines_to_yield.pop(0)

    wrapper = srv._ParseErrorFilteredStdin()
    # _inner starts as None — __anext__ must call _make_inner.
    assert wrapper._inner is None

    with patch.object(wrapper, "_make_inner", return_value=_FakeInner()):
        collected: list[str] = []

        async def _collect() -> None:
            async for line in wrapper:
                collected.append(line)

        asyncio.run(_collect())

    assert len(collected) == 1
