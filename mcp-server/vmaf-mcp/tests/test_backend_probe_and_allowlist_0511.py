# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for ADR-0511 (MCP backend probe + default allowlist).

See `docs/adr/0511-mcp-backend-probe-allowlist-and-ladder-backend.md`
for the cluster write-up.

Bugs covered:

A. ``_list_backends`` returned ``cuda=false`` despite a live CUDA
   binary because the probe grepped ``vmaf --version`` (which does
   not list compiled-in GPU backends on this fork) — must now probe
   ``vmaf --help`` for ``--no_<backend>`` flag presence.

B. Default allowlist excluded the container-side absolute path
   ``/workspace/python/test/resource`` so every container-side MCP
   ``vmaf_score`` call against the Netflix golden YUVs failed
   with "path not under an allowlisted root" unless the caller set
   ``VMAF_MCP_ALLOW`` — the default list must now include the
   container path alongside the host-relative entry.
"""

from __future__ import annotations

import asyncio
from pathlib import Path

import pytest
from vmaf_mcp import server as srv

# ---------------------------------------------------------------------------
# Bug A — backend probe parses --help, not --version
# ---------------------------------------------------------------------------


def _stub_help(stdout: str):
    class _Result:
        def __init__(self) -> None:
            self.stdout = stdout
            self.stderr = ""

    def _runner(*_args, **_kwargs):
        return _Result()

    return _runner


def test_probe_backends_picks_cuda_from_help_when_version_silent(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Bug A: even when ``--version`` says nothing about CUDA, a
    ``--help`` output containing ``--no_cuda`` must yield CUDA=True."""
    fake = tmp_path / "vmaf"
    fake.write_text("#!/bin/sh\nexit 0\n")
    fake.chmod(0o755)
    srv._BACKEND_PROBE_CACHE.pop(str(fake), None)

    # The historical bug was masked by --version being terse; assert
    # _probe_backends does NOT consult --version. We stub subprocess.run
    # to always return a --help blob with --no_cuda listed; if the impl
    # were still calling --version we would see CUDA missing.
    monkeypatch.setattr(
        srv.subprocess,
        "run",
        _stub_help(
            "Usage: vmaf [options]\n"
            "  --no_cuda    disable CUDA backend\n"
            "  --no_hip     disable HIP backend\n"
        ),
    )
    backends = srv._probe_backends(fake)
    assert "cuda" in backends, backends
    assert "hip" in backends, backends
    assert "cpu" in backends, "CPU must always be present"
    # Backends NOT advertised in --help must be absent.
    assert "sycl" not in backends, backends
    assert "metal" not in backends, backends


def test_probe_backends_is_cached_per_binary_path(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Bug A follow-up: the probe must cache its result so the
    server does not fork a vmaf subprocess on every vmaf_score call."""
    fake = tmp_path / "vmaf"
    fake.write_text("#!/bin/sh\nexit 0\n")
    fake.chmod(0o755)
    srv._BACKEND_PROBE_CACHE.pop(str(fake), None)

    call_count = {"n": 0}

    class _Result:
        stdout = "--no_cuda    disable CUDA\n"
        stderr = ""

    def _runner(*_args, **_kwargs):
        call_count["n"] += 1
        return _Result()

    monkeypatch.setattr(srv.subprocess, "run", _runner)
    first = srv._probe_backends(fake)
    second = srv._probe_backends(fake)
    third = srv._probe_backends(fake)
    assert first == second == third
    assert call_count["n"] == 1, (
        f"_probe_backends forked {call_count['n']} subprocesses; " "must cache after the first call"
    )


def test_list_backends_reports_cuda_true_when_help_advertises_it(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Bug A end-to-end: ``_list_backends`` must surface CUDA=True
    when the local vmaf advertises ``--no_cuda`` in --help, even on
    a host whose --version banner omits the word "CUDA"."""
    fake = tmp_path / "vmaf"
    fake.write_text("#!/bin/sh\nexit 0\n")
    fake.chmod(0o755)
    srv._BACKEND_PROBE_CACHE.pop(str(fake), None)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake)
    monkeypatch.setattr(
        srv.subprocess,
        "run",
        _stub_help(
            "Usage: vmaf [options]\n"
            "  --no_cuda    disable CUDA backend\n"
            "  --no_sycl    disable SYCL backend\n"
        ),
    )

    result = asyncio.run(srv._list_backends())
    assert result["cpu"] is True
    assert result["cuda"] is True, f"Bug A regression: {result}"
    assert result["sycl"] is True, result
    assert result["hip"] is False, result
    assert result["metal"] is False, result


# ---------------------------------------------------------------------------
# Bug B — default allowlist includes /workspace/python/test/resource
# ---------------------------------------------------------------------------


def test_default_allowlist_includes_container_python_test_resource() -> None:
    """Bug B: the absolute container path
    ``/workspace/python/test/resource`` must be in the default
    allowlist alongside the host-relative repo path so the canonical
    container-side path for the Netflix golden YUVs works without
    needing ``VMAF_MCP_ALLOW``.
    """
    roots = srv._allowed_roots()
    # Compare as resolved Paths so symlink / trailing-slash drift does
    # not flap the assertion.
    expected = Path("/workspace/python/test/resource").resolve()
    assert expected in roots, (
        "Bug B regression: /workspace/python/test/resource is not in "
        f"default allowlist. roots={roots}"
    )


def test_default_allowlist_still_includes_host_relative_python_test_resource() -> None:
    """Bug B follow-up: adding the container entry must NOT remove
    the host-relative ``<repo>/python/test/resource`` entry — host-side
    invocations (e.g. ``make test``) still depend on it."""
    roots = srv._allowed_roots()
    host_relative = (srv._repo_root() / "python" / "test" / "resource").resolve()
    assert host_relative in roots, (
        "host-relative python/test/resource root is missing — would break "
        f"non-container MCP use. roots={roots}"
    )


def test_vmaf_mcp_allow_env_extends_defaults_not_replaces(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Bug B: the ``VMAF_MCP_ALLOW`` override must be ADDITIVE — it
    extends the default list, does not replace it."""
    extra = tmp_path / "my-extra-root"
    extra.mkdir()
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(extra))
    roots = srv._allowed_roots()
    # Default container entry still present (no replacement).
    assert Path("/workspace/python/test/resource").resolve() in roots, roots
    # Override entry also present (additive).
    assert extra.resolve() in roots, roots
