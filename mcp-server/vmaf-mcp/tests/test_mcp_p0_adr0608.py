# Copyright 2026 Lusoris
"""Regression tests for the four P0 fixes shipped in ADR-0608.

Fixes covered:
  1. isError=False spec-correctness bug — _call_tool must not catch and swallow
     exceptions; the mcp library sets isError=True when the handler raises.
  2. probe_backend tool — runtime health check returning compiled_in + runtime_healthy.
  3. vmaf_version tool — binary identity + build_flags dict.
  4. vmaf_score_encoded tool — decode encoded video via ffmpeg then score.
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock

import pytest
from vmaf_mcp import server as srv

# ---------------------------------------------------------------------------
# 1. isError fix: _call_tool raises instead of swallowing exceptions
# ---------------------------------------------------------------------------


def test_call_tool_raises_on_unknown_tool():
    """_call_tool must raise ValueError for unknown tool names (not return
    a TextContent with isError implicitly False).  The mcp library's outer
    handler is responsible for setting isError=True."""

    async def run():
        return await srv._call_tool("nonexistent_tool_name", {})

    with pytest.raises(ValueError, match="unknown tool"):
        asyncio.run(run())


def test_call_tool_raises_on_bad_path():
    """Path-validation errors must propagate as exceptions, not be caught and
    returned as {'error': ...} TextContent with isError=False."""

    async def run():
        return await srv._call_tool(
            "vmaf_score",
            {
                "ref": "/tmp/definitely_does_not_exist_evil.yuv",
                "dis": "/tmp/also_evil.yuv",
                "width": 576,
                "height": 324,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )

    # Should raise ValueError ("not under an allowlisted root") or
    # FileNotFoundError — either is fine; what matters is that it does NOT
    # silently return a TextContent with isError=False.
    with pytest.raises((ValueError, FileNotFoundError)):
        asyncio.run(run())


# ---------------------------------------------------------------------------
# 2. probe_backend
# ---------------------------------------------------------------------------


def test_probe_backend_cpu_always_compiled_in(monkeypatch):
    """CPU backend is always compiled in; the probe should run a real (mocked)
    vmaf invocation and return runtime_healthy=True on success."""

    async def fake_exec(*args, **kwargs):
        proc = AsyncMock()
        proc.returncode = 0
        proc.communicate = AsyncMock(return_value=(b"", b""))
        return proc

    # Patch asyncio.create_subprocess_exec to avoid spawning a real vmaf process.
    # We also need the output JSON file to be written by the probe.  Rather than
    # trying to intercept the file write, patch _probe_backend's subprocess call
    # and write a stub output file in the TemporaryDirectory it creates.
    import tempfile as _tempfile

    original_tempdir = _tempfile.TemporaryDirectory

    class _FakeTmpDir:
        """Write a stub vmaf JSON into the temp dir so _probe_backend can read it."""

        def __init__(self, *args, **kwargs):
            self._real = original_tempdir(*args, **kwargs)
            self.name = self._real.name

        def __enter__(self):
            path = Path(self._real.__enter__())
            # Pre-write the output JSON that _probe_backend will try to read.
            (path / "out.json").write_text(
                json.dumps({"pooled_metrics": {"vmaf": {"mean": 100.0}}})
            )
            return str(path)

        def __exit__(self, *args):
            return self._real.__exit__(*args)

    monkeypatch.setattr(_tempfile, "TemporaryDirectory", _FakeTmpDir)
    monkeypatch.setattr(asyncio, "create_subprocess_exec", fake_exec)
    # Ensure _vmaf_binary() returns *something* that looks like it exists.
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: Path("/fake/vmaf"))
    # Ensure _probe_backends returns at minimum "cpu" so compiled_in=True.
    monkeypatch.setattr(srv, "_probe_backends", lambda _: frozenset({"cpu"}))

    async def run():
        return await srv._probe_backend("cpu")

    result = asyncio.run(run())

    assert result["backend"] == "cpu"
    assert result["compiled_in"] is True
    assert result["runtime_healthy"] is True
    assert result["score"] == pytest.approx(100.0)
    assert result["error"] is None
    assert result["latency_ms"] is not None and result["latency_ms"] >= 0.0


def test_probe_backend_not_compiled_in_returns_false(monkeypatch):
    """When the backend is not compiled into the local binary, probe_backend
    must return compiled_in=False and runtime_healthy=False without spawning
    a subprocess."""

    monkeypatch.setattr(srv, "_vmaf_binary", lambda: Path("/fake/vmaf"))
    # No backends in the advertised set.
    monkeypatch.setattr(srv, "_probe_backends", lambda _: frozenset({"cpu"}))

    async def run():
        return await srv._probe_backend("cuda")

    result = asyncio.run(run())

    assert result["backend"] == "cuda"
    assert result["compiled_in"] is False
    assert result["runtime_healthy"] is False
    assert "not compiled" in (result["error"] or "")


def test_probe_backend_vmaf_nonzero_exit_returns_healthy_false(monkeypatch):
    """Non-zero exit from vmaf must result in runtime_healthy=False."""

    import tempfile as _tempfile

    original_tempdir = _tempfile.TemporaryDirectory

    class _FakeTmpDir:
        def __init__(self, *args, **kwargs):
            self._real = original_tempdir(*args, **kwargs)
            self.name = self._real.name

        def __enter__(self):
            return str(self._real.__enter__())

        def __exit__(self, *args):
            return self._real.__exit__(*args)

    monkeypatch.setattr(_tempfile, "TemporaryDirectory", _FakeTmpDir)

    async def fake_exec(*args, **kwargs):
        proc = AsyncMock()
        proc.returncode = 1
        proc.communicate = AsyncMock(return_value=(b"", b"driver error: CUDA not available"))
        return proc

    monkeypatch.setattr(asyncio, "create_subprocess_exec", fake_exec)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: Path("/fake/vmaf"))
    monkeypatch.setattr(srv, "_probe_backends", lambda _: frozenset({"cpu", "cuda"}))

    async def run():
        return await srv._probe_backend("cuda")

    result = asyncio.run(run())

    assert result["backend"] == "cuda"
    assert result["compiled_in"] is True
    assert result["runtime_healthy"] is False
    assert result["error"] is not None
    assert "vmaf exited 1" in result["error"]


# ---------------------------------------------------------------------------
# 3. vmaf_version
# ---------------------------------------------------------------------------


def test_vmaf_version_returns_expected_keys(monkeypatch, tmp_path):
    """vmaf_version must return binary_path, version, build_flags, and error=None
    when the binary exists."""

    fake_bin = tmp_path / "vmaf"
    fake_bin.write_text("#!/bin/sh\necho 'vmaf 3.0.0-lusoris.5'\n")
    fake_bin.chmod(0o755)

    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake_bin)
    # Simulate --version output.
    import subprocess as _subprocess

    real_run = _subprocess.run

    def fake_run(cmd, *args, **kwargs):
        if "--version" in cmd:
            r = MagicMock()
            r.returncode = 0
            r.stdout = "vmaf 3.0.0-lusoris.5\n"
            r.stderr = ""
            return r
        return real_run(cmd, *args, **kwargs)

    monkeypatch.setattr(_subprocess, "run", fake_run)
    monkeypatch.setattr(srv, "_probe_backends", lambda _: frozenset({"cpu", "cuda"}))

    result = asyncio.run(srv._vmaf_version())

    assert "binary_path" in result
    assert "version" in result
    assert "build_flags" in result
    assert "error" in result
    assert result["error"] is None
    flags = result["build_flags"]
    assert flags["cpu"] is True
    assert flags["cuda"] is True
    assert flags["sycl"] is False
    assert "3.0.0" in (result["version"] or "")


def test_vmaf_version_missing_binary_returns_error(monkeypatch, tmp_path):
    """When the vmaf binary does not exist, vmaf_version must return an error
    field instead of raising."""

    monkeypatch.setattr(srv, "_vmaf_binary", lambda: tmp_path / "nonexistent_vmaf")

    result = asyncio.run(srv._vmaf_version())

    assert result["error"] is not None
    assert result["version"] is None
    for flag in ("cpu", "cuda", "sycl", "hip", "metal"):
        assert result["build_flags"][flag] is False


# ---------------------------------------------------------------------------
# 4. vmaf_score_encoded
# ---------------------------------------------------------------------------


def test_vmaf_score_encoded_dispatched_in_list_tools():
    """vmaf_score_encoded, probe_backend, and vmaf_version must appear in
    the tool registry returned by _list_tools()."""

    async def get_tools():
        return await srv._list_tools()

    tools = asyncio.run(get_tools())
    names = {t.name for t in tools}
    assert "vmaf_score_encoded" in names, "vmaf_score_encoded missing from tool list"
    assert "probe_backend" in names, "probe_backend missing from tool list"
    assert "vmaf_version" in names, "vmaf_version missing from tool list"


def test_vmaf_score_encoded_ffprobe_not_on_path_raises(monkeypatch, tmp_path):
    """When ffprobe is not installed, _run_vmaf_score_encoded must raise
    RuntimeError — it must not return a success-shaped TextContent."""

    monkeypatch.setattr("shutil.which", lambda _cmd: None)

    fake_video = tmp_path / "ref.mp4"
    fake_video.write_bytes(b"\x00" * 16)

    # Bypass path allowlist for the test.
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    async def run():
        return await srv._run_vmaf_score_encoded(
            ref_path=fake_video,
            dis_path=fake_video,
        )

    with pytest.raises(RuntimeError, match="ffprobe not on PATH"):
        asyncio.run(run())


def test_ffprobe_geometry_parses_stream_correctly(monkeypatch):
    """_ffprobe_geometry must parse ffprobe JSON output and return the
    correct (width, height, pixfmt, bitdepth) tuple."""
    import subprocess as _subprocess

    fake_ffprobe_out = json.dumps(
        {"streams": [{"width": 1920, "height": 1080, "pix_fmt": "yuv420p10le"}]}
    )

    def fake_run(cmd, *args, **kwargs):
        r = MagicMock()
        r.returncode = 0
        r.stdout = fake_ffprobe_out
        r.stderr = ""
        return r

    monkeypatch.setattr(_subprocess, "run", fake_run)
    # Make ffprobe appear to be on PATH.
    import shutil as _shutil

    monkeypatch.setattr(
        _shutil, "which", lambda cmd: "/usr/bin/ffprobe" if cmd == "ffprobe" else None
    )

    width, height, pixfmt, bitdepth = srv._ffprobe_geometry(Path("/fake/video.mp4"))

    assert width == 1920
    assert height == 1080
    assert pixfmt == "420"
    assert bitdepth == 10


def test_ffprobe_geometry_unknown_pixfmt_raises(monkeypatch):
    """Unknown pixel formats must raise ValueError rather than returning
    a wrong bitdepth silently."""
    import shutil as _shutil
    import subprocess as _subprocess

    def fake_run(cmd, *args, **kwargs):
        r = MagicMock()
        r.returncode = 0
        r.stdout = json.dumps({"streams": [{"width": 640, "height": 480, "pix_fmt": "bgr24"}]})
        r.stderr = ""
        return r

    monkeypatch.setattr(_subprocess, "run", fake_run)
    monkeypatch.setattr(
        _shutil, "which", lambda cmd: "/usr/bin/ffprobe" if cmd == "ffprobe" else None
    )

    with pytest.raises(ValueError, match="cannot be mapped"):
        srv._ffprobe_geometry(Path("/fake/video.mp4"))
