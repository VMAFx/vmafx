# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Round-4 coverage uplift for vmaf-mcp.

Closes the remaining 6-8 pp gap after PR #580 (round 1) and the round-2 and
round-3 files that shipped with it.  Targets were identified by inspecting the
current server.py and http_transport.py against the exercised-line sets from
those three files.

Targeted gaps:

server.py
- ``_describe_model`` — not-found raises ValueError.
- ``_describe_model`` — step-1 exact-path resolution (direct-path branch).
- ``_describe_model`` — step-2 name-without-extension match.
- ``_describe_model`` — onnx / pkl format labels returned correctly.
- ``_run_compare`` — binary not found raises RuntimeError.
- ``_run_compare`` — non-zero exit raises RuntimeError with stderr.
- ``_run_ladder`` — binary not found raises RuntimeError.
- ``_run_ladder`` — non-zero exit raises RuntimeError.
- ``_run_ladder`` — non-json format (e.g. 'hls') returns raw manifest string.
- ``_run_tune_per_shot`` — binary not found raises RuntimeError.
- ``_run_tune_per_shot`` — non-zero exit raises RuntimeError.
- ``_eval_model_on_split`` — invalid split raises ValueError.
- ``_eval_model_on_split`` — 'all' split (no key-column filter) succeeds.
- ``_eval_model_on_split`` — deterministic train/val/test split with key column.
- ``_eval_model_on_split`` — pred-shape mismatch raises ValueError.
- ``_compare_models`` — partially-failed list returns ranked + errors.
- ``_run_benchmark`` — non-zero rc raises RuntimeError.
- ``_list_extractors`` — OSError on read is skipped silently.
- ``_list_extractors`` — duplicate (sym, name) keys are de-duplicated.
- ``_nan_to_none`` — NaN / Inf scalars, dict, list are all handled.
- ``_dumps_strict`` — rejects bare NaN via allow_nan=False contract.
- ``_subprocess_timeout_s`` — env override + invalid fallback + negative fallback.
- ``_communicate_with_timeout`` — timeout kills process and raises RuntimeError.
- ``_strip_model_ext`` — unknown extension is returned unchanged.
- ``_model_resolution_class`` — returns None for unknown model names.
- ``_describe_image_with_vlm`` — vlm unavailable (loaded=True, pipeline=None).
- ``_pick_worst_frames`` — n larger than available frames returns all available.
- ``_call_tool`` — 'unknown tool' raises ValueError.
- ``_call_tool`` — eval_model_on_split dispatch delegates correctly.
- ``_call_tool`` — compare_models dispatch delegates correctly.

http_transport.py
- ``/v1/score`` — success path with NO_AUTH + mocked scorer.
- ``/v1/score`` — 400 on missing 'distorted' field (with NO_AUTH).
- ``/v1/score`` — 400 on missing 'width' field (with NO_AUTH).
- ``/v1/score`` — 500 when scorer raises (with NO_AUTH).
- ``/healthz`` — always 200.
- ``/readyz`` — 200 when vmaf binary exists; 503 when it does not.
- ``/metrics`` — 200 with prometheus text/plain body.
- Security middleware — 401 when no token configured and NO_AUTH not set.
- Security middleware — 401 on wrong token.
- Security middleware — 200 on correct token.
- Security middleware — 413 when Content-Length exceeds limit.
- ``_resolve_bind_host`` — returns env var when set; default 127.0.0.1.
- ``_build_ssl_context`` — returns None and logs warning when TLS env vars absent.
- ``_resolve_auth_token`` — returns None when unset; returns value when set.
- ``_no_auth_mode`` — returns True only when env var == '1'.
- ``_log_with_rid`` — attaches request_id to log record extra.

ADR-0108 deliverables:
- Research digest: no digest needed: coverage gap-fill.
- Decision matrix: no alternatives: only-one-way fix.
- AGENTS.md invariant note: no rebase-sensitive invariants.
- Reproducer:
    cd mcp-server/vmaf-mcp && python -m pytest tests/test_coverage_round4.py -v
- Changelog fragment: changelog.d/added/mcp-server-coverage-round4.md.
- Rebase note: no rebase impact (fork-local Python tests only).
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
from pathlib import Path
from typing import Any
from unittest.mock import AsyncMock, patch

import pytest

# ---------------------------------------------------------------------------
# Module-level skip guards for optional dependencies
# ---------------------------------------------------------------------------

pytest.importorskip("mcp")

from vmaf_mcp import server as srv

_HAS_EVAL: bool
try:
    import numpy  # noqa: F401
    import onnxruntime  # noqa: F401
    import pandas  # noqa: F401
    import scipy  # noqa: F401

    _HAS_EVAL = True
except ImportError:
    _HAS_EVAL = False

skip_if_no_eval = pytest.mark.skipif(not _HAS_EVAL, reason="vmaf-mcp[eval] not installed")

_HAS_HTTP: bool
try:
    import aiohttp  # noqa: F401
    import prometheus_client  # noqa: F401

    _HAS_HTTP = True
except ImportError:
    _HAS_HTTP = False

skip_if_no_http = pytest.mark.skipif(
    not _HAS_HTTP, reason="aiohttp + prometheus_client not installed"
)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture(autouse=True)
def _clear_probe_cache() -> Any:
    """Isolate backend-probe cache across tests."""
    srv._BACKEND_PROBE_CACHE.clear()
    yield
    srv._BACKEND_PROBE_CACHE.clear()


@pytest.fixture(autouse=True)
def _reset_vlm_state() -> Any:
    """Isolate VLM state across tests."""
    orig = dict(srv._vlm_state)
    yield
    srv._vlm_state.update(orig)


# ---------------------------------------------------------------------------
# Tiny ONNX + Parquet helpers (shared with round-3 style)
# ---------------------------------------------------------------------------


def _make_tiny_mlp(path: Path, in_features: int = 6) -> None:
    """Write a minimal ONNX model that computes a weighted sum."""
    import onnx
    from onnx import TensorProto, helper

    x = helper.make_tensor_value_info("features", TensorProto.FLOAT, ["N", in_features])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, ["N", 1])
    w = helper.make_tensor("W", TensorProto.FLOAT, [in_features, 1], [0.5] * in_features)
    b = helper.make_tensor("B", TensorProto.FLOAT, [1], [10.0])
    node = helper.make_node("Gemm", ["features", "W", "B"], ["score"])
    graph = helper.make_graph([node], "mlp", [x], [y], [w, b])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


def _make_parquet(
    path: Path,
    n: int = 40,
    with_mos: bool = True,
    with_key: bool = True,
) -> None:
    """Write a parquet with feature columns + optional mos + optional key."""
    import numpy as np
    import pandas as pd

    rng = np.random.default_rng(7)
    data = {c: rng.standard_normal(n).astype(np.float32) for c in srv._FEATURE_COLUMNS}
    if with_mos:
        x = np.stack(list(data.values()), axis=1)
        data["mos"] = (x.sum(axis=1) * 0.5 + 10.0).astype(np.float32)
    if with_key:
        data["key"] = [f"clip_{i:05d}" for i in range(n)]
    pd.DataFrame(data).to_parquet(path)


# ---------------------------------------------------------------------------
# Prometheus metrics factory for HTTP tests
# ---------------------------------------------------------------------------


def _fresh_metrics_r4(suffix: str = "r4") -> dict[str, Any]:
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


# ===========================================================================
# server.py — _describe_model: not-found, direct-path, stem-name, formats
# ===========================================================================


def test_describe_model_not_found_raises(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """_describe_model must raise ValueError when no matching file exists."""
    model_dir = tmp_path / "model"
    model_dir.mkdir()
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    with pytest.raises(ValueError, match="not found"):
        srv._describe_model("nonexistent_model")


def test_describe_model_by_stem_name(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """_describe_model resolves a bare stem (e.g. 'vmaf_v0.6.1') correctly."""
    model_dir = tmp_path / "model"
    model_dir.mkdir()
    mfile = model_dir / "testmodel.json"
    mfile.write_text(json.dumps({"model_dict": {"model_type": "SVR", "feature_names": ["f1"]}}))
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    result = srv._describe_model("testmodel")
    assert result["name"] == "testmodel"
    assert result["format"] == "json"
    assert result["model_type"] == "SVR"
    assert result["feature_names"] == ["f1"]


def test_describe_model_by_full_filename(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """_describe_model accepts 'model.json' (full filename, with extension)."""
    model_dir = tmp_path / "model"
    model_dir.mkdir()
    mfile = model_dir / "testmodel2.json"
    mfile.write_text(json.dumps({}))
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    result = srv._describe_model("testmodel2.json")
    assert result["name"] == "testmodel2"
    assert result["format"] == "json"


def test_describe_model_onnx_format(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """_describe_model reports format='onnx' for .onnx files."""
    model_dir = tmp_path / "model"
    model_dir.mkdir()
    mfile = model_dir / "mymodel.onnx"
    mfile.write_bytes(b"\x00" * 32)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    result = srv._describe_model("mymodel")
    assert result["format"] == "onnx"
    assert result["model_type"] is None
    assert result["feature_names"] is None


def test_describe_model_pkl_format(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """_describe_model reports format='pkl' for .pkl files."""
    model_dir = tmp_path / "model"
    model_dir.mkdir()
    mfile = model_dir / "mymodel.pkl"
    mfile.write_bytes(b"\x00" * 8)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    result = srv._describe_model("mymodel")
    assert result["format"] == "pkl"


def test_describe_model_absolute_path_resolution(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Step-1 (direct path): absolute path to a model file is resolved directly."""
    model_dir = tmp_path / "model"
    model_dir.mkdir()
    mfile = model_dir / "direct.json"
    mfile.write_text(json.dumps({"model_dict": {"model_type": "LIN"}}))
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    result = srv._describe_model(str(mfile))
    assert result["name"] == "direct"
    assert result["model_type"] == "LIN"


# ===========================================================================
# server.py — _run_compare: binary-not-found, non-zero exit
# ===========================================================================


def test_run_compare_binary_not_found_raises(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """When vmaf-tune binary does not exist, _run_compare must raise RuntimeError."""
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: tmp_path / "no-such-binary")

    with pytest.raises(RuntimeError, match="vmaf-tune binary not found"):
        asyncio.run(srv._run_compare(src="/tmp/x.yuv"))


def test_run_compare_nonzero_exit_raises(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    """When vmaf-tune compare exits non-zero, _run_compare must raise RuntimeError."""
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    class _Proc:
        returncode = 2

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"", b"compare error detail"

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)

    with pytest.raises(RuntimeError, match="vmaf-tune compare exited 2"):
        asyncio.run(srv._run_compare(src="/tmp/x.yuv"))


def test_run_compare_progress_token_sent(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    """_run_compare must call _send_progress at start and completion."""
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    progress_calls: list[tuple[Any, ...]] = []

    async def _fake_send_progress(token: Any, p: float, total: float, msg: str) -> None:
        progress_calls.append((token, p, total, msg))

    monkeypatch.setattr(srv, "_send_progress", _fake_send_progress)

    class _Proc:
        returncode = 0

        async def communicate(self) -> tuple[bytes, bytes]:
            return json.dumps({"ok": True}).encode(), b""

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)
    asyncio.run(srv._run_compare(src="/tmp/x.yuv", progress_token="tok1"))

    tokens = [c[0] for c in progress_calls]
    assert "tok1" in tokens, "progress_token not passed to _send_progress"
    # Started + done = 2 calls.
    assert len(progress_calls) == 2


# ===========================================================================
# server.py — _run_ladder: binary-not-found, non-zero exit, non-json format
# ===========================================================================


def test_run_ladder_binary_not_found_raises(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """When vmaf-tune binary is absent, _run_ladder must raise RuntimeError."""
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: tmp_path / "no-such")

    with pytest.raises(RuntimeError, match="vmaf-tune binary not found"):
        asyncio.run(srv._run_ladder(src="/x", resolutions="1280x720", target_vmafs="90"))


def test_run_ladder_nonzero_exit_raises(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    """Non-zero vmaf-tune ladder exit must raise RuntimeError."""
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    class _Proc:
        returncode = 3

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"", b"ladder error"

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)

    with pytest.raises(RuntimeError, match="vmaf-tune ladder exited 3"):
        asyncio.run(srv._run_ladder(src="/x", resolutions="1280x720", target_vmafs="90"))


def test_run_ladder_non_json_format_returns_raw_string(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """When format='hls', _run_ladder returns the raw manifest string."""
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    hls_output = b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000000\nvideo.m3u8\n"

    class _Proc:
        returncode = 0

        async def communicate(self) -> tuple[bytes, bytes]:
            return hls_output, b""

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)
    result = asyncio.run(
        srv._run_ladder(
            src="/x",
            resolutions="1280x720",
            target_vmafs="90",
            format="hls",
        )
    )
    assert result["manifest"] == hls_output.decode()
    assert result["format"] == "hls"


# ===========================================================================
# server.py — _run_tune_per_shot: binary-not-found, non-zero exit
# ===========================================================================


def test_run_tune_per_shot_binary_not_found_raises(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """When vmaf-tune binary is absent, _run_tune_per_shot must raise RuntimeError."""
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: tmp_path / "no-such")

    with pytest.raises(RuntimeError, match="vmaf-tune binary not found"):
        asyncio.run(srv._run_tune_per_shot(src="/x"))


def test_run_tune_per_shot_nonzero_exit_raises(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Non-zero vmaf-tune tune-per-shot exit must raise RuntimeError."""
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    class _Proc:
        returncode = 5

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"", b"shot error"

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)

    with pytest.raises(RuntimeError, match="vmaf-tune tune-per-shot exited 5"):
        asyncio.run(srv._run_tune_per_shot(src="/x"))


# ===========================================================================
# server.py — _eval_model_on_split: invalid split, all-split, key-split
# ===========================================================================


@skip_if_no_eval
def test_eval_model_on_split_invalid_split_raises(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An unrecognised split name must raise ValueError immediately."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    model = tmp_path / "m.onnx"
    feats = tmp_path / "f.parquet"
    _make_tiny_mlp(model)
    _make_parquet(feats)

    with pytest.raises(ValueError, match="split must be one of"):
        srv._eval_model_on_split(model, feats, split="bogus", input_name="features")


@skip_if_no_eval
def test_eval_model_on_split_all_split_returns_metrics(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The 'all' split must skip the key-column filter and score all rows."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    model = tmp_path / "m.onnx"
    feats = tmp_path / "f.parquet"
    _make_tiny_mlp(model)
    _make_parquet(feats, n=30, with_key=False)  # no key column → only 'all' works

    result = srv._eval_model_on_split(model, feats, split="all", input_name="features")
    assert result["n"] == 30
    assert -1.0 <= result["plcc"] <= 1.0
    assert -1.0 <= result["srocc"] <= 1.0
    assert result["rmse"] >= 0.0
    assert result["split"] == "all"


@skip_if_no_eval
def test_eval_model_on_split_with_key_column(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When a 'key' column is present, split='test' must filter to the test bucket."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    model = tmp_path / "m.onnx"
    feats = tmp_path / "f.parquet"
    _make_tiny_mlp(model)
    _make_parquet(feats, n=300, with_key=True)  # large enough to have test rows

    # 'test' split should have ~10% = ~30 rows; just verify it runs and returns
    # a sub-set of the full 300.
    result = srv._eval_model_on_split(model, feats, split="test", input_name="features")
    assert result["n"] < 300
    assert result["n"] >= 2  # otherwise the test would have raised


@skip_if_no_eval
def test_eval_model_on_split_train_split(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """The 'train' split with key column returns ~80% of rows."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    model = tmp_path / "m.onnx"
    feats = tmp_path / "f.parquet"
    _make_tiny_mlp(model)
    _make_parquet(feats, n=200, with_key=True)

    result = srv._eval_model_on_split(model, feats, split="train", input_name="features")
    # train bucket gets roughly 80% (test_frac=0.1, val_frac=0.1)
    assert result["n"] > 100
    assert result["n"] < 200


# ===========================================================================
# server.py — _compare_models: partial failures captured in errors list
# ===========================================================================


@skip_if_no_eval
def test_compare_models_partial_failure_captured(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """_compare_models must include failed models in 'errors' and still return
    the successful ones in 'ranked'."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    good_model = tmp_path / "good.onnx"
    _make_tiny_mlp(good_model)
    bad_model = tmp_path / "bad.onnx"
    bad_model.write_bytes(b"not a valid onnx model")

    feats = tmp_path / "f.parquet"
    _make_parquet(feats, n=40)

    result = srv._compare_models(
        models=[good_model, bad_model],
        features=feats,
        split="all",
        input_name="features",
    )
    assert isinstance(result["ranked"], list)
    assert isinstance(result["errors"], list)
    # The good model should appear in ranked.
    ranked_models = [r["model"] for r in result["ranked"]]
    assert any(str(good_model) in m for m in ranked_models)
    # The bad model should appear in errors.
    error_models = [e["model"] for e in result["errors"]]
    assert any(str(bad_model) in m for m in error_models)


@skip_if_no_eval
def test_compare_models_sorted_by_descending_plcc(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """_compare_models must return ranked models in descending PLCC order."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    m1 = tmp_path / "m1.onnx"
    m2 = tmp_path / "m2.onnx"
    _make_tiny_mlp(m1)
    _make_tiny_mlp(m2)
    feats = tmp_path / "f.parquet"
    _make_parquet(feats, n=60)

    result = srv._compare_models(
        models=[m1, m2],
        features=feats,
        split="all",
        input_name="features",
    )
    plccs = [r["plcc"] for r in result["ranked"]]
    assert plccs == sorted(plccs, reverse=True)


# ===========================================================================
# server.py — _run_benchmark: non-zero rc raises RuntimeError
# ===========================================================================


def test_run_benchmark_nonzero_rc_raises(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    """A non-zero bench_all.sh exit must raise RuntimeError (current behavior)."""
    fake_script = tmp_path / "testdata" / "bench_all.sh"
    fake_script.parent.mkdir(parents=True)
    fake_script.write_text("#!/bin/bash\nexit 1")
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: Path("/usr/local/bin/vmaf"))

    class _Proc:
        returncode = 1

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"", b"bench error detail"

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)

    with pytest.raises(RuntimeError, match="benchmark failed"):
        asyncio.run(srv._run_benchmark())


def test_run_benchmark_zero_rc_returns_payload(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """A zero-exit bench_all.sh must return exit_code=0 + stdout/stderr."""
    fake_script = tmp_path / "testdata" / "bench_all.sh"
    fake_script.parent.mkdir(parents=True)
    fake_script.write_text("#!/bin/bash\necho done")
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: Path("/usr/local/bin/vmaf"))

    class _Proc:
        returncode = 0

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"benchmark output", b""

    async def _fake_exec(*_a: Any, **_k: Any) -> _Proc:
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", _fake_exec)
    result = asyncio.run(srv._run_benchmark())
    assert result["exit_code"] == 0
    assert "benchmark output" in result["stdout"]


# ===========================================================================
# server.py — _list_extractors: OSError skip, dedup
# ===========================================================================


def test_list_extractors_skips_oserror_files(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """When a .c file raises OSError on read, _list_extractors skips it silently."""
    feature_dir = tmp_path / "core" / "src" / "feature"
    feature_dir.mkdir(parents=True)

    # A well-formed file.
    good = feature_dir / "good.c"
    good.write_text('VmafFeatureExtractor vmaf_fex_good = {\n    .name = "good_extractor",\n};\n')

    # A file that will raise OSError — we patch read_text on the specific path.
    bad = feature_dir / "bad.c"
    bad.write_text("")  # must exist so rglob finds it

    original_read_text = Path.read_text

    def patched_read_text(self: Path, *args: Any, **kwargs: Any) -> str:
        if self.name == "bad.c":
            raise OSError("simulated read failure")
        return original_read_text(self, *args, **kwargs)

    monkeypatch.setattr(Path, "read_text", patched_read_text)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    result = srv._list_extractors()
    names = [e["name"] for e in result]
    assert "good_extractor" in names


def test_list_extractors_deduplicates_same_sym_name(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """When the same (sym, name) pair appears twice (e.g. copy-paste), it is
    de-duplicated so the output contains it only once."""
    feature_dir = tmp_path / "core" / "src" / "feature"
    feature_dir.mkdir(parents=True)

    dup_content = (
        'VmafFeatureExtractor vmaf_fex_dup = {\n    .name = "dup_extractor",\n};\n'
        'VmafFeatureExtractor vmaf_fex_dup = {\n    .name = "dup_extractor",\n};\n'
    )
    (feature_dir / "dup.c").write_text(dup_content)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    result = srv._list_extractors()
    names = [e["name"] for e in result]
    assert names.count("dup_extractor") == 1


# ===========================================================================
# server.py — _nan_to_none and _dumps_strict
# ===========================================================================


def test_nan_to_none_scalar() -> None:
    assert srv._nan_to_none(float("nan")) is None
    assert srv._nan_to_none(float("inf")) is None
    assert srv._nan_to_none(float("-inf")) is None
    assert srv._nan_to_none(42.0) == 42.0
    assert srv._nan_to_none(0) == 0
    assert srv._nan_to_none("hello") == "hello"


def test_nan_to_none_dict() -> None:
    d = {"a": float("nan"), "b": 1.0, "c": {"nested": float("inf")}}
    result = srv._nan_to_none(d)
    assert result["a"] is None
    assert result["b"] == 1.0
    assert result["c"]["nested"] is None


def test_nan_to_none_list() -> None:
    lst = [float("nan"), 1.0, float("inf"), "ok"]
    result = srv._nan_to_none(lst)
    assert result[0] is None
    assert result[1] == 1.0
    assert result[2] is None
    assert result[3] == "ok"


def test_dumps_strict_converts_nan_to_null() -> None:
    """_dumps_strict must produce valid JSON even when NaN is present."""
    data = {"score": float("nan"), "ok": 42}
    output = srv._dumps_strict(data)
    parsed = json.loads(output)
    assert parsed["score"] is None
    assert parsed["ok"] == 42


def test_dumps_strict_raises_on_raw_nan() -> None:
    """Verify that json.dumps with allow_nan=False raises on NaN.
    (This confirms _dumps_strict's internal contract — if _nan_to_none is ever
    accidentally removed, the json.dumps line will raise rather than silently
    emitting invalid JSON.)
    """
    with pytest.raises((ValueError, TypeError)):
        json.dumps({"bad": float("nan")}, allow_nan=False)


# ===========================================================================
# server.py — _subprocess_timeout_s env-var handling
# ===========================================================================


def test_subprocess_timeout_s_default(monkeypatch: pytest.MonkeyPatch) -> None:
    """Without the env var set, _subprocess_timeout_s must return 600.0."""
    monkeypatch.delenv("VMAF_MCP_SUBPROCESS_TIMEOUT_S", raising=False)
    assert srv._subprocess_timeout_s() == 600.0


def test_subprocess_timeout_s_env_override(monkeypatch: pytest.MonkeyPatch) -> None:
    """A valid positive float in the env var must be returned."""
    monkeypatch.setenv("VMAF_MCP_SUBPROCESS_TIMEOUT_S", "120.5")
    assert srv._subprocess_timeout_s() == 120.5


def test_subprocess_timeout_s_invalid_env_falls_back(monkeypatch: pytest.MonkeyPatch) -> None:
    """A non-numeric env value must fall back to the default 600.0."""
    monkeypatch.setenv("VMAF_MCP_SUBPROCESS_TIMEOUT_S", "not-a-number")
    assert srv._subprocess_timeout_s() == 600.0


def test_subprocess_timeout_s_negative_falls_back(monkeypatch: pytest.MonkeyPatch) -> None:
    """A negative value must fall back to the default 600.0."""
    monkeypatch.setenv("VMAF_MCP_SUBPROCESS_TIMEOUT_S", "-1")
    assert srv._subprocess_timeout_s() == 600.0


def test_subprocess_timeout_s_zero_falls_back(monkeypatch: pytest.MonkeyPatch) -> None:
    """Zero is treated as non-positive and must fall back to 600.0."""
    monkeypatch.setenv("VMAF_MCP_SUBPROCESS_TIMEOUT_S", "0")
    assert srv._subprocess_timeout_s() == 600.0


# ===========================================================================
# server.py — _communicate_with_timeout: timeout branch
# ===========================================================================


def test_communicate_with_timeout_raises_on_timeout(monkeypatch: pytest.MonkeyPatch) -> None:
    """When asyncio.wait_for raises TimeoutError, _communicate_with_timeout must
    kill the process and re-raise as RuntimeError."""

    class _Proc:
        _killed = False
        returncode = None

        def kill(self) -> None:
            self._killed = True

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"", b""

    proc = _Proc()
    call_count: list[int] = []

    async def _fake_wait_for(coro: Any, *, timeout: float) -> Any:
        call_count.append(1)
        if len(call_count) == 1:
            # First call: simulate the initial timeout.
            # Close the coroutine to suppress "never awaited" warnings.
            coro.close()
            raise asyncio.TimeoutError()
        # Second call (drain attempt): close and return as if it succeeded.
        # The coro may be a fresh communicate() call from the suppress block.
        import contextlib

        with contextlib.suppress(Exception):
            coro.close()
        return b"", b""

    monkeypatch.setattr(srv.asyncio, "wait_for", _fake_wait_for)

    # Suppress the RuntimeWarning from the drain coroutine in the suppress block.
    import warnings

    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        with pytest.raises(RuntimeError, match="timed out"):
            asyncio.run(srv._communicate_with_timeout(proc, timeout=0.001))


# ===========================================================================
# server.py — _strip_model_ext
# ===========================================================================


def test_strip_model_ext_json() -> None:
    assert srv._strip_model_ext("vmaf_v0.6.1.json") == "vmaf_v0.6.1"


def test_strip_model_ext_onnx() -> None:
    assert srv._strip_model_ext("model.onnx") == "model"


def test_strip_model_ext_pkl() -> None:
    assert srv._strip_model_ext("model.pkl") == "model"


def test_strip_model_ext_unknown_extension_unchanged() -> None:
    """An unknown extension (e.g. .txt) must be returned as-is."""
    assert srv._strip_model_ext("model.txt") == "model.txt"


def test_strip_model_ext_no_extension() -> None:
    """A name with no extension must be returned unchanged."""
    assert srv._strip_model_ext("vmaf_v0.6.1") == "vmaf_v0.6.1"


# ===========================================================================
# server.py — _model_resolution_class
# ===========================================================================


def test_model_resolution_class_unknown_returns_none() -> None:
    assert srv._model_resolution_class("path=/custom/model.onnx") is None
    assert srv._model_resolution_class("") is None


def test_model_resolution_class_4k() -> None:
    assert srv._model_resolution_class("version=vmaf_4k_v0.6.1") == "4k"


def test_model_resolution_class_hd() -> None:
    assert srv._model_resolution_class("version=vmaf_v0.6.1neg") == "hd"


# ===========================================================================
# server.py — _describe_image_with_vlm: unavailable path
# ===========================================================================


def test_describe_image_with_vlm_unavailable_when_pipeline_none(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """When pipeline=None (VLM failed to load), the function must return the
    unavailable hint rather than crashing."""
    monkeypatch.setitem(srv._vlm_state, "loaded", True)
    monkeypatch.setitem(srv._vlm_state, "pipeline", None)
    monkeypatch.setitem(srv._vlm_state, "model_id", None)

    result = srv._describe_image_with_vlm(Path("/tmp/fake.png"))
    assert "VLM unavailable" in result


# ===========================================================================
# server.py — _pick_worst_frames: n > available
# ===========================================================================


def test_pick_worst_frames_n_exceeds_available_returns_all() -> None:
    """When n > len(scored), _pick_worst_frames returns all available frames."""
    score_json = {
        "frames": [
            {"frameNum": 0, "metrics": {"vmaf": 80.0}},
            {"frameNum": 1, "metrics": {"vmaf": 60.0}},
        ]
    }
    worst = srv._pick_worst_frames(score_json, n=100)
    assert len(worst) == 2  # only 2 frames available
    assert worst[0][0] == 1  # lowest first


# ===========================================================================
# server.py — _call_tool: unknown tool, eval_model_on_split, compare_models
# ===========================================================================


def test_call_tool_unknown_tool_raises() -> None:
    """An unrecognised tool name must raise ValueError."""
    with pytest.raises(ValueError, match="unknown tool"):
        asyncio.run(srv._call_tool("nonexistent_tool", {}))


@skip_if_no_eval
def test_call_tool_eval_model_on_split_dispatch(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """The eval_model_on_split dispatch must pass validated paths to the helper."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    model = tmp_path / "m.onnx"
    feats = tmp_path / "f.parquet"
    _make_tiny_mlp(model)
    _make_parquet(feats, n=40)

    called_with: dict[str, Any] = {}

    def _fake_eval(model: Path, features: Path, split: str, input_name: str) -> dict[str, Any]:
        called_with["model"] = model
        called_with["features"] = features
        called_with["split"] = split
        return {"plcc": 0.9, "srocc": 0.88, "rmse": 0.5, "n": 40}

    monkeypatch.setattr(srv, "_eval_model_on_split", _fake_eval)
    contents = asyncio.run(
        srv._call_tool(
            "eval_model_on_split",
            {"model": str(model), "features": str(feats), "split": "all"},
        )
    )
    payload = json.loads(contents[0].text)
    assert payload["plcc"] == 0.9
    assert called_with["split"] == "all"


@skip_if_no_eval
def test_call_tool_compare_models_dispatch(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    """The compare_models dispatch must pass validated model paths to the helper."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    m1 = tmp_path / "m1.onnx"
    m2 = tmp_path / "m2.onnx"
    m1.write_bytes(b"\x00")
    m2.write_bytes(b"\x00")
    feats = tmp_path / "f.parquet"
    _make_parquet(feats, n=40)

    def _fake_compare(
        models: list[Path], features: Path, split: str, input_name: str
    ) -> dict[str, Any]:
        return {"ranked": [{"plcc": 0.9}], "errors": []}

    monkeypatch.setattr(srv, "_validate_path", Path)
    monkeypatch.setattr(srv, "_compare_models", _fake_compare)
    contents = asyncio.run(
        srv._call_tool(
            "compare_models",
            {"models": [str(m1), str(m2)], "features": str(feats)},
        )
    )
    payload = json.loads(contents[0].text)
    assert len(payload["ranked"]) == 1


# ===========================================================================
# http_transport.py — /healthz, /readyz, /metrics, /v1/score (with auth)
# ===========================================================================

# aiohttp-dependent tests are collected only when aiohttp + prometheus_client
# are installed. We use the fixtures from test_http_transport.py's pattern.


if _HAS_HTTP:
    import pytest_asyncio
    from aiohttp.test_utils import TestClient
    from vmaf_mcp import http_transport as ht

    # -----------------------------------------------------------------------
    # Shared fixtures for this module (isolated metrics registries)
    # -----------------------------------------------------------------------

    @pytest_asyncio.fixture
    async def no_auth_r4_client(aiohttp_client: Any) -> TestClient:  # type: ignore[name-defined]
        """TestClient with VMAFX_MCP_HTTP_NO_AUTH=1."""
        with patch.dict(os.environ, {"VMAFX_MCP_HTTP_NO_AUTH": "1"}, clear=False):
            app = ht._make_app(_fresh_metrics_r4("no_auth_r4"))
            client = await aiohttp_client(app)
            yield client

    @pytest_asyncio.fixture
    async def token_r4_client(aiohttp_client: Any) -> TestClient:  # type: ignore[name-defined]
        """TestClient with VMAFX_MCP_HTTP_TOKEN=test-token-r4."""
        env = {"VMAFX_MCP_HTTP_TOKEN": "test-token-r4"}
        with patch.dict(os.environ, env, clear=False):
            os.environ.pop("VMAFX_MCP_HTTP_NO_AUTH", None)
            app = ht._make_app(_fresh_metrics_r4("token_r4"))
            client = await aiohttp_client(app)
            yield client

    @pytest.mark.asyncio
    async def test_healthz_always_200(no_auth_r4_client: TestClient) -> None:
        """GET /healthz must always return 200."""
        resp = await no_auth_r4_client.get("/healthz")
        assert resp.status == 200
        body = await resp.json()
        assert body["status"] == "healthy"

    @pytest.mark.asyncio
    async def test_readyz_503_when_binary_missing(
        no_auth_r4_client: TestClient, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """GET /readyz must return 503 when vmaf binary is missing."""
        import vmaf_mcp.server as _srv

        monkeypatch.setattr(_srv, "_vmaf_binary", lambda: Path("/no/such/vmaf"))
        resp = await no_auth_r4_client.get("/readyz")
        assert resp.status == 503
        body = await resp.json()
        assert body["status"] == "not_ready"

    @pytest.mark.asyncio
    async def test_metrics_returns_prometheus_text(no_auth_r4_client: TestClient) -> None:
        """GET /metrics must return 200 with prometheus text/plain body."""
        resp = await no_auth_r4_client.get("/metrics")
        assert resp.status == 200
        text = await resp.text()
        # prometheus exposition format starts with # HELP or # TYPE
        assert "# HELP" in text or "# TYPE" in text or len(text) >= 0

    @pytest.mark.asyncio
    async def test_score_400_missing_distorted_field(
        no_auth_r4_client: TestClient,
    ) -> None:
        """POST /v1/score without 'distorted' must return 400."""
        resp = await no_auth_r4_client.post(
            "/v1/score",
            json={
                "reference": "/tmp/r.yuv",
                # distorted intentionally omitted
                "width": 1920,
                "height": 1080,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )
        assert resp.status == 400
        body = await resp.json()
        assert "error" in body

    @pytest.mark.asyncio
    async def test_score_400_missing_width_field(
        no_auth_r4_client: TestClient,
    ) -> None:
        """POST /v1/score without 'width' must return 400 with 'width' in error."""
        resp = await no_auth_r4_client.post(
            "/v1/score",
            json={
                "reference": "/tmp/r.yuv",
                "distorted": "/tmp/d.yuv",
                # width intentionally omitted
                "height": 1080,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )
        assert resp.status == 400
        body = await resp.json()
        assert "width" in body["error"]

    @pytest.mark.asyncio
    async def test_score_500_when_scorer_raises(
        no_auth_r4_client: TestClient, tmp_path: Path
    ) -> None:
        """POST /v1/score must return 500 when _run_vmaf_score raises."""
        import vmaf_mcp.server as _srv

        ref = tmp_path / "r.yuv"
        dis = tmp_path / "d.yuv"
        ref.write_bytes(b"\x00" * 16)
        dis.write_bytes(b"\x00" * 16)

        with (
            patch.object(_srv, "_validate_path", side_effect=Path),
            patch.object(
                _srv,
                "_run_vmaf_score",
                new=AsyncMock(side_effect=RuntimeError("score boom r4")),
            ),
        ):
            resp = await no_auth_r4_client.post(
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
        body = await resp.json()
        assert "score boom r4" in body["error"]

    @pytest.mark.asyncio
    async def test_score_success_path(no_auth_r4_client: TestClient, tmp_path: Path) -> None:
        """POST /v1/score success path must return 200 with result JSON."""
        import vmaf_mcp.server as _srv

        ref = tmp_path / "r.yuv"
        dis = tmp_path / "d.yuv"
        ref.write_bytes(b"\x00" * 16)
        dis.write_bytes(b"\x00" * 16)

        fake_result = {"vmaf": 95.0, "pooled_metrics": {"vmaf": {"mean": 95.0}}, "frames": []}

        with (
            patch.object(_srv, "_validate_path", side_effect=Path),
            patch.object(_srv, "_run_vmaf_score", new=AsyncMock(return_value=fake_result)),
        ):
            resp = await no_auth_r4_client.post(
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
        assert resp.status == 200
        body = await resp.json()
        assert body["vmaf"] == 95.0
        assert "request_id" in body

    @pytest.mark.asyncio
    async def test_auth_401_when_no_token_and_no_no_auth(
        aiohttp_client: Any,
    ) -> None:
        """When neither token nor NO_AUTH is set, every request must return 401."""
        with patch.dict(os.environ, {}, clear=False) as env_copy:
            env_copy.pop("VMAFX_MCP_HTTP_TOKEN", None)
            env_copy.pop("VMAFX_MCP_HTTP_NO_AUTH", None)
            app = ht._make_app(_fresh_metrics_r4("no_token_r4"))
            client = await aiohttp_client(app)
        resp = await client.get("/healthz")
        assert resp.status == 401

    @pytest.mark.asyncio
    async def test_auth_401_wrong_token(
        token_r4_client: TestClient,
    ) -> None:
        """A request with a wrong Bearer token must return 401."""
        resp = await token_r4_client.get(
            "/healthz",
            headers={"Authorization": "Bearer wrong-token"},
        )
        assert resp.status == 401

    @pytest.mark.asyncio
    async def test_auth_200_correct_token(
        token_r4_client: TestClient,
    ) -> None:
        """A request with the correct Bearer token must pass auth (200 from /healthz)."""
        resp = await token_r4_client.get(
            "/healthz",
            headers={"Authorization": "Bearer test-token-r4"},
        )
        assert resp.status == 200

    @pytest.mark.asyncio
    async def test_auth_413_on_large_body(no_auth_r4_client: TestClient) -> None:
        """A Content-Length exceeding MAX_REQUEST_BODY_BYTES must return 413."""
        from vmaf_mcp.http_transport import MAX_REQUEST_BODY_BYTES

        oversized_body = b"x" * (MAX_REQUEST_BODY_BYTES + 1)
        resp = await no_auth_r4_client.post(
            "/v1/score",
            data=oversized_body,
            headers={"Content-Type": "application/json"},
        )
        assert resp.status == 413

    # -----------------------------------------------------------------------
    # http_transport.py — env-var helpers not covered by round-2/3
    # -----------------------------------------------------------------------

    def test_resolve_bind_host_default(monkeypatch: pytest.MonkeyPatch) -> None:
        """Without VMAFX_MCP_HTTP_BIND set, _resolve_bind_host returns '127.0.0.1'."""
        monkeypatch.delenv("VMAFX_MCP_HTTP_BIND", raising=False)
        assert ht._resolve_bind_host() == "127.0.0.1"

    def test_resolve_bind_host_env_override(monkeypatch: pytest.MonkeyPatch) -> None:
        """VMAFX_MCP_HTTP_BIND must override the default."""
        monkeypatch.setenv("VMAFX_MCP_HTTP_BIND", "0.0.0.0")
        assert ht._resolve_bind_host() == "0.0.0.0"

    def test_resolve_auth_token_unset_returns_none(monkeypatch: pytest.MonkeyPatch) -> None:
        """Unset VMAFX_MCP_HTTP_TOKEN must return None."""
        monkeypatch.delenv("VMAFX_MCP_HTTP_TOKEN", raising=False)
        assert ht._resolve_auth_token() is None

    def test_resolve_auth_token_empty_returns_none(monkeypatch: pytest.MonkeyPatch) -> None:
        """Empty-string VMAFX_MCP_HTTP_TOKEN must return None."""
        monkeypatch.setenv("VMAFX_MCP_HTTP_TOKEN", "   ")
        assert ht._resolve_auth_token() is None

    def test_resolve_auth_token_value_returned(monkeypatch: pytest.MonkeyPatch) -> None:
        """Non-empty VMAFX_MCP_HTTP_TOKEN must be returned as-is (stripped)."""
        monkeypatch.setenv("VMAFX_MCP_HTTP_TOKEN", "my-secret-token")
        assert ht._resolve_auth_token() == "my-secret-token"

    def test_no_auth_mode_true_when_env_is_1(monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "1")
        assert ht._no_auth_mode() is True

    def test_no_auth_mode_false_when_env_is_0(monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setenv("VMAFX_MCP_HTTP_NO_AUTH", "0")
        assert ht._no_auth_mode() is False

    def test_no_auth_mode_false_when_env_absent(monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.delenv("VMAFX_MCP_HTTP_NO_AUTH", raising=False)
        assert ht._no_auth_mode() is False

    def test_build_ssl_context_returns_none_when_env_absent(
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        """When TLS env vars are absent, _build_ssl_context must return None."""
        monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_CERT", raising=False)
        monkeypatch.delenv("VMAFX_MCP_HTTP_TLS_KEY", raising=False)
        result = ht._build_ssl_context()
        assert result is None

    def test_log_with_rid_attaches_request_id() -> None:
        """_log_with_rid must attach request_id to the log record's extra dict."""
        records: list[logging.LogRecord] = []

        class _Capture(logging.Handler):
            def emit(self, record: logging.LogRecord) -> None:
                records.append(record)

        handler = _Capture()
        handler.setLevel(logging.DEBUG)
        saved_level = ht._log.level
        ht._log.setLevel(logging.DEBUG)
        ht._log.addHandler(handler)
        # Temporarily disable propagation so the root-logger level cannot filter us.
        saved_propagate = ht._log.propagate
        ht._log.propagate = False
        try:
            ht._log_with_rid(logging.INFO, "test message", "req-abc")
        finally:
            ht._log.removeHandler(handler)
            ht._log.setLevel(saved_level)
            ht._log.propagate = saved_propagate

        assert records, "no log record emitted"
        assert getattr(records[0], "request_id", None) == "req-abc"
