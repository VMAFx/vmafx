# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for B1 (_vmaf_binary path resolution) and B2 (_run_benchmark env injection).

ADR-0517: ``_run_benchmark`` takes NO positional arguments (bench_all.sh is a
fixed-fixture suite); it injects VMAF_ROOT and VMAF_BIN into the subprocess
environment and passes zero positional args to the script.
"""

from __future__ import annotations

import asyncio
from pathlib import Path

from vmaf_mcp import server as srv

# ---------------------------------------------------------------------------
# B1: _vmaf_binary() path resolution
# ---------------------------------------------------------------------------


def test_vmaf_binary_honours_env_var(monkeypatch, tmp_path):
    """VMAF_BIN env var is the highest-priority override."""
    fake_bin = tmp_path / "my-vmaf"
    fake_bin.touch()
    monkeypatch.setenv("VMAF_BIN", str(fake_bin))
    assert srv._vmaf_binary() == fake_bin


def test_vmaf_binary_prefers_usr_local(monkeypatch, tmp_path):
    """When /usr/local/bin/vmaf exists it is preferred over in-tree builds."""
    monkeypatch.delenv("VMAF_BIN", raising=False)
    if Path("/usr/local/bin/vmaf").exists():
        result = srv._vmaf_binary()
        assert result == Path("/usr/local/bin/vmaf")
    else:
        monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)
        result = srv._vmaf_binary()
        assert "core" in str(result)


def test_vmaf_binary_falls_back_to_core_build(monkeypatch, tmp_path):
    """When /usr/local/bin/vmaf is absent the core/build path is next."""
    monkeypatch.delenv("VMAF_BIN", raising=False)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    core_bin = tmp_path / "core" / "build" / "tools" / "vmaf"
    core_bin.parent.mkdir(parents=True)
    core_bin.touch()

    original_exists = Path.exists

    def patched_exists(self):
        if str(self) == "/usr/local/bin/vmaf":
            return False
        return original_exists(self)

    monkeypatch.setattr(Path, "exists", patched_exists)

    result = srv._vmaf_binary()
    assert result == core_bin


def test_vmaf_binary_falls_back_to_core_when_all_absent(monkeypatch, tmp_path):
    """When no candidate exists, returns core/build path as the best guess."""
    monkeypatch.delenv("VMAF_BIN", raising=False)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    original_exists = Path.exists

    def patched_exists(self):
        if str(self) == "/usr/local/bin/vmaf" or str(self).startswith(str(tmp_path)):
            return False
        return original_exists(self)

    monkeypatch.setattr(Path, "exists", patched_exists)

    result = srv._vmaf_binary()
    assert "core" in str(result) and "build" in str(result)


# ---------------------------------------------------------------------------
# B2: _run_benchmark env injection (ADR-0517)
# ---------------------------------------------------------------------------


def _make_bench_script(tmp_path):
    """Create a minimal bench_all.sh stub in tmp_path/testdata/."""
    (tmp_path / "testdata").mkdir(exist_ok=True)
    bench_script = tmp_path / "testdata" / "bench_all.sh"
    bench_script.write_text("#!/bin/sh\nexit 0\n")
    bench_script.chmod(0o755)
    return bench_script


def test_run_benchmark_injects_vmaf_root(monkeypatch, tmp_path):
    """_run_benchmark must pass VMAF_ROOT=<repo_root> to the subprocess so
    bench_all.sh resolves its cd correctly in environments where git is absent
    or the hard-coded host path does not exist."""
    import anyio

    captured_env: dict[str, str] = {}

    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)
    _make_bench_script(tmp_path)

    async def fake_cse(*args, **kwargs):
        captured_env.update(kwargs.get("env") or {})

        class _FakeProc:
            returncode = 0

            async def communicate(self):
                return b"", b""

        return _FakeProc()

    monkeypatch.setattr(asyncio, "create_subprocess_exec", fake_cse)

    anyio.run(srv._run_benchmark)

    assert "VMAF_ROOT" in captured_env, (
        "_run_benchmark did not pass VMAF_ROOT to the subprocess env; "
        "bench_all.sh will fail in containers where git is unavailable."
    )
    assert captured_env["VMAF_ROOT"] == str(
        tmp_path
    ), f"VMAF_ROOT={captured_env['VMAF_ROOT']!r} should equal repo root {tmp_path!r}"


def test_run_benchmark_injects_vmaf_bin(monkeypatch, tmp_path):
    """_run_benchmark must pass VMAF_BIN so bench_all.sh does not fall back to
    the relative in-tree path (core/build/tools/vmaf) that is absent in
    the container after ``make install`` (ADR-0513 root cause #2)."""
    import anyio

    captured_env: dict[str, str] = {}
    fake_bin = tmp_path / "fake-vmaf"
    fake_bin.touch()

    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake_bin)
    _make_bench_script(tmp_path)

    async def fake_cse(*args, **kwargs):
        captured_env.update(kwargs.get("env") or {})

        class _FakeProc:
            returncode = 0

            async def communicate(self):
                return b"", b""

        return _FakeProc()

    monkeypatch.setattr(asyncio, "create_subprocess_exec", fake_cse)

    anyio.run(srv._run_benchmark)

    assert "VMAF_BIN" in captured_env, (
        "_run_benchmark did not pass VMAF_BIN to the subprocess env; "
        "bench_all.sh will silently use the relative in-tree fallback path."
    )
    assert captured_env["VMAF_BIN"] == str(
        fake_bin
    ), f"VMAF_BIN={captured_env['VMAF_BIN']!r} should equal {fake_bin!r}"


def test_run_benchmark_passes_no_positional_args(monkeypatch, tmp_path):
    """bench_all.sh must be invoked with NO positional arguments.

    Passing ``-r ref -d dis --width W --height H`` corrupts the ``$@`` array
    visible inside the sourced Intel oneAPI ``setvars.sh``, which processes
    ``$@`` as its own flags. Unknown flags are forwarded to per-component
    ``env/vars.sh`` scripts, which may hang or exit non-zero — aborting
    bench_all.sh before any output is emitted (ADR-0517 root cause #1).
    """
    import anyio

    captured_args: list[str] = []

    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)
    _make_bench_script(tmp_path)

    async def fake_cse(*args, **kwargs):
        captured_args.extend(args)

        class _FakeProc:
            returncode = 0

            async def communicate(self):
                return b"", b""

        return _FakeProc()

    monkeypatch.setattr(asyncio, "create_subprocess_exec", fake_cse)

    anyio.run(srv._run_benchmark)

    # captured_args[0] is the script path; there must be nothing after it.
    assert len(captured_args) == 1, (
        f"bench_all.sh must be called with zero positional args; "
        f"got {captured_args[1:]!r}. Extra args corrupt $@ inside "
        "the sourced oneAPI setvars.sh (ADR-0517)."
    )


def test_run_benchmark_preserves_inherited_env(monkeypatch, tmp_path):
    """VMAF_ROOT/VMAF_BIN injection must not discard the process environment --
    PATH and GPU runtime vars must survive."""
    import anyio

    captured_env: dict[str, str] = {}
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)
    _make_bench_script(tmp_path)

    monkeypatch.setenv("SENTINEL_VAR", "sentinel_value")

    async def fake_cse(*args, **kwargs):
        captured_env.update(kwargs.get("env") or {})

        class _FakeProc:
            returncode = 0

            async def communicate(self):
                return b"", b""

        return _FakeProc()

    monkeypatch.setattr(asyncio, "create_subprocess_exec", fake_cse)

    anyio.run(srv._run_benchmark)

    assert captured_env.get("SENTINEL_VAR") == "sentinel_value", (
        "_run_benchmark must preserve the inherited environment; "
        "PATH / GPU runtime vars would be lost otherwise."
    )
