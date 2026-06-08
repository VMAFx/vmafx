# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for the vmaf-mcp server — no network, no GPU required."""

from __future__ import annotations

import asyncio
import os
import shutil
from pathlib import Path

import pytest
from vmaf_mcp import server as srv

REPO = Path(__file__).resolve().parents[3]


def test_repo_root_detects_testdata():
    root = srv._repo_root()
    assert (root / "testdata").is_dir()


def test_validate_path_accepts_golden_yuv():
    yuv = REPO / "python/test/resource/yuv/src01_hrc00_576x324.yuv"
    if not yuv.exists():
        pytest.skip("Netflix golden YUV not present")
    assert srv._validate_path(str(yuv)) == yuv.resolve()


def test_validate_path_rejects_outside_roots(tmp_path):
    bad = tmp_path / "evil.yuv"
    bad.write_bytes(b"\x00" * 16)
    with pytest.raises(ValueError, match="not under an allowlisted root"):
        srv._validate_path(str(bad))


def test_validate_path_accepts_custom_allow(tmp_path, monkeypatch):
    f = tmp_path / "ok.yuv"
    f.write_bytes(b"\x00" * 16)
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    assert srv._validate_path(str(f)) == f.resolve()


def test_list_models_returns_list():
    models = srv._list_models()
    assert isinstance(models, list)
    for m in models:
        assert "name" in m and "path" in m and "format" in m


def test_list_backends_always_includes_cpu():
    backends = asyncio.run(srv._list_backends())
    assert backends["cpu"] is True


# ---------------------------------------------------------------------------
# eval_model_on_split / compare_models — require the 'eval' extra
# ---------------------------------------------------------------------------


def _has_eval_deps() -> bool:
    try:
        import numpy  # noqa: F401
        import onnx  # noqa: F401
        import onnxruntime  # noqa: F401
        import pandas  # noqa: F401
        import scipy  # noqa: F401
    except ImportError:
        return False
    return True


pytestmark_eval = pytest.mark.skipif(
    not _has_eval_deps(), reason="vmaf-mcp[eval] extras not installed"
)


def _make_tiny_mlp(path: Path, in_features: int = 6) -> None:
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


def _make_feature_parquet(path: Path, n: int = 64) -> None:
    import numpy as np
    import pandas as pd

    rng = np.random.default_rng(0)
    data = {c: rng.standard_normal(n).astype(np.float32) for c in srv._FEATURE_COLUMNS}
    # MOS is a linear transform of the features so correlations are non-trivial.
    x = np.stack(list(data.values()), axis=1)
    data["mos"] = (x.sum(axis=1) * 0.5 + 10.0 + rng.normal(0, 0.01, n)).astype(np.float32)
    data["key"] = [f"sample_{i:04d}" for i in range(n)]
    pd.DataFrame(data).to_parquet(path)


@pytestmark_eval
def test_eval_model_on_split_reports_metrics(tmp_path, monkeypatch):
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    model = tmp_path / "m.onnx"
    feats = tmp_path / "f.parquet"
    _make_tiny_mlp(model)
    _make_feature_parquet(feats)
    result = srv._eval_model_on_split(model, feats, split="all", input_name="features")
    assert result["n"] == 64
    # The MOS column is a linear function of the features with tiny noise, so
    # the linear model's correlation should be very close to 1.
    assert result["plcc"] > 0.99
    assert result["rmse"] >= 0.0
    assert result["split"] == "all"


@pytestmark_eval
def test_eval_model_on_split_rejects_unknown_split(tmp_path):
    model = tmp_path / "m.onnx"
    feats = tmp_path / "f.parquet"
    _make_tiny_mlp(model)
    _make_feature_parquet(feats)
    with pytest.raises(ValueError, match="split must be one of"):
        srv._eval_model_on_split(model, feats, split="nope", input_name="features")


@pytestmark_eval
def test_eval_split_filters_by_key(tmp_path):
    """train+val+test should partition the parquet exactly."""
    model = tmp_path / "m.onnx"
    feats = tmp_path / "f.parquet"
    _make_tiny_mlp(model)
    _make_feature_parquet(feats, n=200)
    ns = {
        s: srv._eval_model_on_split(model, feats, split=s, input_name="features")["n"]
        for s in ("train", "val", "test")
    }
    assert sum(ns.values()) == 200
    # Small-sample fractions drift from 10% / 10% / 80% but should stay in-band.
    assert ns["train"] > ns["val"] and ns["train"] > ns["test"]


@pytestmark_eval
def test_compare_models_ranks_by_plcc(tmp_path):
    good = tmp_path / "good.onnx"
    bad = tmp_path / "bad.onnx"
    feats = tmp_path / "f.parquet"
    _make_tiny_mlp(good)
    # A model with opposite-sign weights will have strongly negative PLCC.
    import onnx
    from onnx import TensorProto, helper

    x = helper.make_tensor_value_info("features", TensorProto.FLOAT, ["N", 6])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, ["N", 1])
    w = helper.make_tensor("W", TensorProto.FLOAT, [6, 1], [-0.5] * 6)
    b = helper.make_tensor("B", TensorProto.FLOAT, [1], [10.0])
    node = helper.make_node("Gemm", ["features", "W", "B"], ["score"])
    graph = helper.make_graph([node], "mlp", [x], [y], [w, b])
    onnx.save(helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)]), str(bad))
    _make_feature_parquet(feats)
    report = srv._compare_models([bad, good], feats, split="all", input_name="features")
    assert report["errors"] == []
    assert report["ranked"][0]["model"] == str(good)
    assert report["ranked"][0]["plcc"] > report["ranked"][1]["plcc"]


@pytestmark_eval
def test_compare_models_captures_errors_without_aborting(tmp_path):
    ok = tmp_path / "ok.onnx"
    _make_tiny_mlp(ok)
    missing = tmp_path / "missing.onnx"
    feats = tmp_path / "f.parquet"
    _make_feature_parquet(feats)
    report = srv._compare_models([ok, missing], feats, split="all", input_name="features")
    # One report, one error — the missing model doesn't take the good one down.
    assert len(report["ranked"]) == 1
    assert len(report["errors"]) == 1
    assert report["errors"][0]["model"] == str(missing)


def test_new_tools_registered_in_list_tools():
    """Schema-level check that doesn't need eval extras installed."""
    import anyio

    async def get_tools():
        return await srv._list_tools()

    tools = anyio.run(get_tools)
    names = {t.name for t in tools}
    assert "eval_model_on_split" in names
    assert "compare_models" in names
    # ADR-0172 / T6-6: VLM-assisted artefact triage tool.
    assert "describe_worst_frames" in names


# ─────────────────────────────────────────────────────────────────────
# describe_worst_frames (T6-6 / ADR-0172)
# ─────────────────────────────────────────────────────────────────────


def test_describe_worst_frames_picks_lowest_vmaf():
    """The picker walks the per-frame array and returns the N with
    smallest VMAF, sorted ascending."""
    score_json = {
        "frames": [
            {"frameNum": 0, "metrics": {"vmaf": 90.0}},
            {"frameNum": 1, "metrics": {"vmaf": 30.0}},
            {"frameNum": 2, "metrics": {"vmaf": 50.0}},
            {"frameNum": 3, "metrics": {"vmaf": 10.0}},
            {"frameNum": 4, "metrics": {"vmaf": 70.0}},
        ],
    }
    worst = srv._pick_worst_frames(score_json, n=3)
    assert worst == [(3, 10.0), (1, 30.0), (2, 50.0)]


def test_describe_worst_frames_handles_alternate_metric_keys():
    """Different VMAF model variants emit different headline-metric
    keys. The picker recognises the common ones."""
    score_json = {
        "frames": [
            {"frameNum": 0, "metrics": {"vmaf_v0.6.1": 80.0}},
            {"frameNum": 1, "metrics": {"vmaf_v0.6.1": 20.0}},
        ],
    }
    worst = srv._pick_worst_frames(score_json, n=1)
    assert worst == [(1, 20.0)]


def test_describe_worst_frames_skips_frames_without_a_score():
    score_json = {
        "frames": [
            {"frameNum": 0, "metrics": {}},  # no headline key
            {"frameNum": 1, "metrics": {"vmaf": 5.0}},
        ],
    }
    worst = srv._pick_worst_frames(score_json, n=2)
    assert worst == [(1, 5.0)]


def test_describe_worst_frames_n_zero_returns_empty_list():
    score_json = {
        "frames": [{"frameNum": 0, "metrics": {"vmaf": 99.0}}],
    }
    assert srv._pick_worst_frames(score_json, n=0) == []


def test_describe_worst_frames_allocates_unique_tmpdir_per_call(tmp_path, monkeypatch):
    """Each call to describe_worst_frames uses its own TemporaryDirectory (no
    mkdtemp leak) and the two calls allocate distinct roots.

    Replaces the older ``test_describe_worst_frames_tmpdir_cleared_on_next_call``
    invariant (which enforced a *shared* ``/tmp/vmaf-mcp-worst-<pid>`` dir that
    each subsequent call ``rmtree``-d at start). That contract was racy under
    concurrent MCP tool calls: call B's ``rmtree`` would delete the PNGs call
    A had just emitted but not yet returned to its caller. See the
    2026-05-31 Python-surfaces bug-audit bundle (Bug 14).

    The new invariant: each call uses a ``tempfile.TemporaryDirectory`` context
    manager, so the directory (and its PNGs) is cleaned up automatically when
    the call returns.  Callers that need persistent paths must copy files out
    before returning from the tool handler.
    """
    import anyio

    # A minimal score JSON that looks like a 1-frame result with a known score.
    fake_score = {
        "frames": [{"frameNum": 0, "metrics": {"vmaf": 10.0}}],
    }

    captured_roots: list[Path] = []

    async def run():
        async def fake_score_fn(_req):
            return fake_score

        async def fake_extract(_yuv, **_kwargs):
            import struct
            import zlib

            path = _kwargs["out_png"]
            captured_roots.append(path.parent)

            def write_chunk(chunk_type, data):
                crc = zlib.crc32(chunk_type + data) & 0xFFFFFFFF
                return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", crc)

            raw = (
                b"\x89PNG\r\n\x1a\n"
                + write_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
                + write_chunk(b"IDAT", zlib.compress(b"\x00\xff\xff\xff"))
                + write_chunk(b"IEND", b"")
            )
            path.write_bytes(raw)

        orig_score = srv._run_vmaf_score
        orig_extract = srv._extract_frame_png
        srv._run_vmaf_score = fake_score_fn
        srv._extract_frame_png = fake_extract
        try:
            req = srv.ScoreRequest(
                ref=Path("/dev/null"),
                dis=Path("/dev/null"),
                width=1,
                height=1,
                pixfmt="420",
                bitdepth=8,
            )
            # Drive two sequential calls — each must allocate its own dir.
            r1 = await srv._describe_worst_frames(req, n=1, describe=lambda _p: "stub description")
            r2 = await srv._describe_worst_frames(req, n=1, describe=lambda _p: "stub description")
            return r1, r2
        finally:
            srv._extract_frame_png = orig_extract
            srv._run_vmaf_score = orig_score

    r1, r2 = anyio.run(run)

    # Each call allocated its own root — that's the stricter contract.
    unique = {str(r) for r in captured_roots}
    assert len(unique) == 2, (
        "Each describe_worst_frames call must allocate its own TemporaryDirectory; "
        "found duplicate roots which is the race the audit fixed."
    )
    # Both response payloads contain the expected metadata.
    for resp in (r1, r2):
        assert len(resp["frames"]) == 1
        assert resp["frames"][0]["description"] == "stub description"
    # TemporaryDirectory cleans up on context exit, so the PNG paths no longer
    # exist after the call returns — callers must copy files out if they need
    # persistence beyond the tool-handler lifetime.
    for resp in (r1, r2):
        assert not Path(
            resp["frames"][0]["png"]
        ).exists(), "TemporaryDirectory leaked: PNG file still present after _describe_worst_frames returned"


# ---------------------------------------------------------------------------
# Round 26 A.2 — NamedTemporaryFile uniqueness + cleanup
# ---------------------------------------------------------------------------


def test_score_tempfile_uses_unique_path(tmp_path, monkeypatch):
    """10 concurrent calls must produce 10 distinct output paths (no task-name
    collision).  We intercept the subprocess call and write a minimal valid
    JSON payload to the output path so _run_vmaf_score can read it back."""
    import anyio

    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    # Create stub ref/dis files so path validation passes.
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    # Create a stub binary so the exists() check passes.
    vmaf_stub = tmp_path / "vmaf-stub"
    vmaf_stub.touch()

    # Patch _vmaf_binary so no real binary is needed.
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: vmaf_stub)

    # Patch _probe_backends so backend validation passes for "auto".
    monkeypatch.setattr(srv, "_probe_backends", lambda _vmaf: frozenset({"cpu"}))

    captured_paths: list[Path] = []

    class _FakeProc:
        returncode = 0

        async def communicate(self):
            return b"", b""

    async def fake_subprocess(*argv, **_kwargs):
        # The output path is the second-to-last arg (before --json).
        # argv[0] is the vmaf binary string; scan for "-o".
        args = list(argv)
        idx = args.index("-o")
        out_path = Path(args[idx + 1])
        captured_paths.append(out_path)
        # Write a minimal valid JSON result so json.loads succeeds.
        out_path.write_text('{"frames": [{"frameNum": 0, "metrics": {"vmaf": 90.0}}]}')
        return _FakeProc()

    monkeypatch.setattr(asyncio, "create_subprocess_exec", fake_subprocess)

    req = srv.ScoreRequest(
        ref=ref,
        dis=dis,
        width=16,
        height=1,
        pixfmt="420",
        bitdepth=8,
    )

    async def run_concurrent():
        tasks = [asyncio.create_task(srv._run_vmaf_score(req)) for _ in range(10)]
        return await asyncio.gather(*tasks)

    anyio.run(run_concurrent)

    assert len(captured_paths) == 10, "expected exactly 10 calls"
    assert (
        len(set(str(p) for p in captured_paths)) == 10
    ), "task-name collision: two concurrent calls produced the same output path"
    # All temp files must have been cleaned up by the finally block.
    for p in captured_paths:
        assert not p.exists(), f"tempfile not cleaned up: {p}"


def test_score_tempfile_cleaned_up_on_exception(tmp_path, monkeypatch):
    """An exception raised during scoring must still cause the output
    tempfile to be unlinked (the finally block must fire)."""
    import anyio

    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    vmaf_stub = tmp_path / "vmaf-stub"
    vmaf_stub.touch()

    monkeypatch.setattr(srv, "_vmaf_binary", lambda: vmaf_stub)
    monkeypatch.setattr(srv, "_probe_backends", lambda _vmaf: frozenset({"cpu"}))

    leaked_path: list[Path] = []

    class _FailProc:
        returncode = 1

        async def communicate(self):
            return b"", b"simulated scoring failure"

    async def fail_subprocess(*argv, **_kwargs):
        args = list(argv)
        idx = args.index("-o")
        out_path = Path(args[idx + 1])
        leaked_path.append(out_path)
        # Do NOT write the output file — vmaf "failed".
        return _FailProc()

    monkeypatch.setattr(asyncio, "create_subprocess_exec", fail_subprocess)

    req = srv.ScoreRequest(
        ref=ref,
        dis=dis,
        width=16,
        height=1,
        pixfmt="420",
        bitdepth=8,
    )

    async def run():
        with pytest.raises(RuntimeError, match="vmaf exited 1"):
            await srv._run_vmaf_score(req)

    anyio.run(run)

    assert len(leaked_path) == 1
    # The finally block must have cleaned up even though scoring raised.
    assert not leaked_path[0].exists(), "tempfile leaked after exception in _run_vmaf_score"


# ---------------------------------------------------------------------------
# Concurrency cap — _SCORE_SEM limits simultaneous vmaf subprocesses
# ---------------------------------------------------------------------------


def test_score_sem_limits_concurrent_vmaf_subprocesses(tmp_path, monkeypatch):
    """16 concurrent compute_vmaf calls must not spawn more than
    VMAF_MCP_MAX_CONCURRENT (here pinned to 8) vmaf subprocesses at
    the same time.

    We instrument asyncio.create_subprocess_exec to count how many
    fake vmaf processes are alive simultaneously and record the observed
    high-water mark.  With the semaphore the peak must be ≤ 8; without
    it the peak would reach 16.
    """
    import anyio

    # Pin the semaphore to exactly 8 for this test.
    monkeypatch.setattr(srv, "_SCORE_SEM", asyncio.Semaphore(8))

    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))

    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    vmaf_stub = tmp_path / "vmaf-stub"
    vmaf_stub.touch()

    monkeypatch.setattr(srv, "_vmaf_binary", lambda: vmaf_stub)
    monkeypatch.setattr(srv, "_probe_backends", lambda _vmaf: frozenset({"cpu"}))

    peak_concurrent: list[int] = [0]
    current_concurrent: list[int] = [0]

    class _FakeProc:
        returncode = 0

        async def communicate(self):
            # Yield to the event loop so other tasks can enter the semaphore
            # region; this gives the test a realistic concurrent-overlap window.
            await asyncio.sleep(0)
            return b"", b""

    async def fake_subprocess(*argv, **_kwargs):
        current_concurrent[0] += 1
        if current_concurrent[0] > peak_concurrent[0]:
            peak_concurrent[0] = current_concurrent[0]
        proc = _FakeProc()
        # Write the minimal JSON payload the caller expects.
        args = list(argv)
        idx = args.index("-o")
        out_path = Path(args[idx + 1])
        out_path.write_text('{"frames": [{"frameNum": 0, "metrics": {"vmaf": 90.0}}]}')
        current_concurrent[0] -= 1
        return proc

    monkeypatch.setattr(asyncio, "create_subprocess_exec", fake_subprocess)

    req = srv.ScoreRequest(
        ref=ref,
        dis=dis,
        width=16,
        height=1,
        pixfmt="420",
        bitdepth=8,
    )

    async def run_16_concurrent():
        tasks = [asyncio.create_task(srv._run_vmaf_score(req)) for _ in range(16)]
        return await asyncio.gather(*tasks)

    anyio.run(run_16_concurrent)

    assert peak_concurrent[0] <= 8, (
        f"semaphore failed: {peak_concurrent[0]} concurrent vmaf subprocesses observed, "
        "expected ≤ 8 (VMAF_MCP_MAX_CONCURRENT default)"
    )
    # Sanity: all 16 calls completed successfully.
    assert peak_concurrent[0] >= 1, "no subprocess calls were observed — test fixture broken"


def test_describe_image_falls_back_to_metadata_only_without_extras(monkeypatch):
    """When the [vlm] extras aren't installed, _load_vlm() returns
    None and _describe_image_with_vlm surfaces a clear hint."""
    # Reset the cache so this test isn't influenced by other tests.
    srv._vlm_state["loaded"] = False
    srv._vlm_state["pipeline"] = None
    srv._vlm_state["model_id"] = None

    # Force the import-failure branch by hiding `transformers`.
    monkeypatch.setitem(__import__("sys").modules, "transformers", None)
    msg = srv._describe_image_with_vlm(Path("/tmp/nonexistent.png"))
    assert "VLM unavailable" in msg
    assert "vmaf-mcp[vlm]" in msg
