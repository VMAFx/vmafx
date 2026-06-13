# Copyright 2026 Lusoris
"""Regression tests for the five MCP probe findings of 2026-05-17.

See `docs/adr/0495-mcp-probe-bug-fixes.md` for the cluster write-up.

Bugs covered:
  1. Silent backend fallback (`backend="cuda"` on CPU-only build) — must
     now raise *and* the success path must echo `backend_used`.
  2. Tool schema enum dropped `hip` / `metal` — schema must advertise
     every supported backend (post-ADR-0726: auto, cpu, cuda, sycl,
     hip, metal) for both `vmaf_score` and `describe_worst_frames`.
  3. `run_benchmark` returning `exit_code=1` with empty stdout AND
     stderr — wrapper must surface a meaningful `error` field.
  5. `vmaf_4k_v0.6.1` on 576×324 silently saturates — wrapper must
     populate `mismatched_model_warning`.

Bug #4 (ref==dis ≠ 100) is documented as a model artefact in the ADR
and is not amenable to a code-side fix without breaking the Netflix
golden gate, so this file does not assert against it.
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from unittest.mock import AsyncMock, patch

import pytest
from vmaf_mcp import server as srv

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_score_request(
    backend: str = "auto",
    model: str = "version=vmaf_v0.6.1",
    width: int = 576,
    height: int = 324,
) -> srv.ScoreRequest:
    return srv.ScoreRequest(
        ref=Path("/tmp/ref.yuv"),
        dis=Path("/tmp/dis.yuv"),
        width=width,
        height=height,
        pixfmt="420",
        bitdepth=8,
        model=model,
        backend=backend,
        precision="17",
    )


def _install_fake_vmaf(tmp_path: Path, monkeypatch, advertised: tuple[str, ...]) -> Path:
    """Install a fake vmaf binary that advertises @p advertised backends
    via its --help output, and wire `_vmaf_binary` to point at it."""
    fake = tmp_path / "vmaf"
    fake.write_text("#!/bin/sh\nexit 0\n")
    fake.chmod(0o755)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: fake)

    # Pre-seed the probe cache so we don't actually shell out.
    backends = {"cpu", *advertised}
    monkeypatch.setitem(srv._BACKEND_PROBE_CACHE, str(fake), frozenset(backends))
    return fake


def _run_score_with_fake_proc(
    req: srv.ScoreRequest,
    *,
    payload: dict,
) -> dict:
    async def _fake_communicate(*args, **kwargs):
        return (b"", b"")

    fake_proc = AsyncMock()
    fake_proc.communicate = _fake_communicate
    fake_proc.returncode = 0

    with (
        patch.object(srv.asyncio, "create_subprocess_exec", AsyncMock(return_value=fake_proc)),
        patch.object(srv.Path, "read_text", return_value=json.dumps(payload)),
        patch.object(srv.Path, "unlink", return_value=None),
    ):
        return asyncio.run(srv._run_vmaf_score(req))


# ---------------------------------------------------------------------------
# Bug #1 — silent backend fallback
# ---------------------------------------------------------------------------


def test_bug1_unavailable_backend_raises(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Requesting a backend the local binary does not advertise must
    raise — never silently fall back to CPU."""
    _install_fake_vmaf(tmp_path, monkeypatch, advertised=())  # CPU-only
    req = _make_score_request(backend="cuda")
    with pytest.raises(RuntimeError, match="backend 'cuda' requested"):
        _run_score_with_fake_proc(req, payload={"frames": [], "pooled_metrics": {}})


def test_bug1_available_backend_passes_through(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When the backend IS advertised, the call succeeds and the response
    echoes both the requested and the (here equal) used backend."""
    _install_fake_vmaf(tmp_path, monkeypatch, advertised=("cuda",))
    req = _make_score_request(backend="cuda")
    result = _run_score_with_fake_proc(
        req,
        payload={"frames": [{"metrics": {"vmaf": 90.0}}], "pooled_metrics": {}},
    )
    assert result["backend_requested"] == "cuda"
    assert result["backend_used"] == "cuda"


def test_bug1_auto_infers_backend_from_payload(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """For `backend='auto'`, `backend_used` is inferred from the JSON
    key-count signature documented in `bench_all.sh::compare`.
    Post-ADR-0726 (Vulkan dropped 2026-05-28) the small-metrics-count
    path maps to the generic 'gpu' label rather than 'vulkan'."""
    _install_fake_vmaf(tmp_path, monkeypatch, advertised=("cuda",))
    req = _make_score_request(backend="auto")
    # GPU-style payload — first-frame metrics has 8 keys (<= 12).
    small_metrics = {f"k{i}": 1.0 for i in range(8)}
    result = _run_score_with_fake_proc(
        req,
        payload={"frames": [{"metrics": small_metrics}], "pooled_metrics": {}},
    )
    assert result["backend_requested"] == "auto"
    assert result["backend_used"] == "gpu"


# ---------------------------------------------------------------------------
# Bug #2 — schema enum dropped hip/metal (vulkan dropped by ADR-0726)
# ---------------------------------------------------------------------------


def test_bug2_schema_advertises_all_supported_backends() -> None:
    """Both `vmaf_score` and `describe_worst_frames` must enumerate every
    supported backend in their JSON-schema enum (post-ADR-0726)."""
    tools = asyncio.run(srv._list_tools())
    by_name = {t.name: t for t in tools}
    expected = {"auto", "cpu", "cuda", "sycl", "hip", "metal"}

    for tool_name in ("vmaf_score", "describe_worst_frames"):
        tool = by_name[tool_name]
        backend_schema = tool.inputSchema["properties"]["backend"]
        assert (
            set(backend_schema["enum"]) == expected
        ), f"{tool_name}: enum is {backend_schema['enum']}, expected {sorted(expected)}"


# ---------------------------------------------------------------------------
# Bug #3 — run_benchmark empty-output failure surfacing
# ---------------------------------------------------------------------------


def test_bug3_run_benchmark_surfaces_silent_pipefail(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When bench_all.sh exits non-zero with empty stdout AND stderr
    (the classic `set -euo pipefail` silent-abort), _run_benchmark must
    raise RuntimeError with a diagnostic that includes "no output" —
    so the MCP layer can mark the call as isError=True (ADR-0608 E-1).

    The OLD _run_benchmark (pre-ADR-0608) returned a success-shaped dict
    with an ``error`` key, which the MCP layer could not distinguish from
    a healthy result.  The new implementation raises RuntimeError so the
    MCP surface correctly sets isError=True.
    """
    # Stub script existence — the wrapper file-existence-checks it before exec.
    (tmp_path / "testdata").mkdir()
    bench_script = tmp_path / "testdata" / "bench_all.sh"
    bench_script.write_text("#!/bin/sh\nexit 1\n")
    bench_script.chmod(0o755)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    class _FakeProc:
        returncode = 1

        async def communicate(self):
            return b"", b""

    monkeypatch.setattr(asyncio, "create_subprocess_exec", AsyncMock(return_value=_FakeProc()))

    with pytest.raises(RuntimeError, match="benchmark failed.*no output"):
        asyncio.run(srv._run_benchmark())


# ---------------------------------------------------------------------------
# Bug #5 — resolution-mismatch warning
# ---------------------------------------------------------------------------


def test_bug5_mismatched_model_warning_on_4k_model_sd_source(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """`vmaf_4k_v0.6.1` on 576×324 input must populate
    `mismatched_model_warning` in the response."""
    _install_fake_vmaf(tmp_path, monkeypatch, advertised=())
    req = _make_score_request(
        backend="auto",
        model="version=vmaf_4k_v0.6.1",
        width=576,
        height=324,
    )
    result = _run_score_with_fake_proc(
        req,
        payload={"frames": [{"metrics": {"vmaf": 100.0}}], "pooled_metrics": {}},
    )
    assert "mismatched_model_warning" in result, result
    assert "'4k'" in result["mismatched_model_warning"]
    assert "576x324" in result["mismatched_model_warning"]


def test_bug5_no_warning_when_model_matches_source(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Matching model+source must NOT emit a warning (no false positives)."""
    _install_fake_vmaf(tmp_path, monkeypatch, advertised=())
    # vmaf_v0.6.1 is the HD-trained model — pair with 1920x1080 source.
    req = _make_score_request(
        backend="auto",
        model="version=vmaf_v0.6.1",
        width=1920,
        height=1080,
    )
    result = _run_score_with_fake_proc(
        req,
        payload={"frames": [{"metrics": {"vmaf": 90.0}}], "pooled_metrics": {}},
    )
    assert "mismatched_model_warning" not in result, result


def test_bug5_no_warning_for_unknown_models(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Bespoke ONNX models (not in _MODEL_RES_HINTS) must NOT trigger the
    warning — we don't have a resolution preset to compare against."""
    _install_fake_vmaf(tmp_path, monkeypatch, advertised=())
    req = _make_score_request(
        backend="auto",
        model="path=/some/custom_model.onnx",
        width=576,
        height=324,
    )
    result = _run_score_with_fake_proc(
        req,
        payload={"frames": [{"metrics": {"vmaf": 50.0}}], "pooled_metrics": {}},
    )
    assert "mismatched_model_warning" not in result, result


# ---------------------------------------------------------------------------
# Backend-probe helper — direct unit tests
# ---------------------------------------------------------------------------


def test_probe_backends_parses_help_output(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """`_probe_backends` must pick out advertised `--no_<backend>` flags
    from vmaf's --help output."""
    fake = tmp_path / "vmaf"
    fake.write_text("#!/bin/sh\nexit 0\n")
    fake.chmod(0o755)
    srv._BACKEND_PROBE_CACHE.pop(str(fake), None)

    class _R:
        stdout = "--no_cuda    disable CUDA\n--no_hip     disable HIP\n"
        stderr = ""

    def _fake_run(*args, **kwargs):
        return _R()

    monkeypatch.setattr(srv.subprocess, "run", _fake_run)
    backends = srv._probe_backends(fake)
    assert backends == frozenset({"cpu", "cuda", "hip"}), backends
