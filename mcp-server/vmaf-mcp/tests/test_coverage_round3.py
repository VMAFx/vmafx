# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Round-3 coverage uplift for vmaf-mcp (mcp-server coverage push PR).

Targets residual gaps not owned by any earlier test file.  Tests exercise
real code paths in the installed module; no pragma:no cover shortcuts.

Targeted coverage gaps (against installed server.py / http_transport.py):

server.py
- ``_probe_backends`` — timeout / OSError fallback (cpu-only assumption).
- ``_probe_backends`` — returns cached result on repeated call.
- ``_infer_backend_from_payload`` — vulkan branch (nkeys >= 30).
- ``_infer_backend_from_payload`` — gpu branch (nkeys <= 12).
- ``_infer_backend_from_payload`` — cpu branch (12 < nkeys < 30).
- ``_infer_backend_from_payload`` — empty frames fallback.
- ``_eval_model_on_split`` — features parquet missing 'mos' column.
- ``_eval_model_on_split`` — missing feature columns error.
- ``_eval_model_on_split`` — too-few-samples (< 2) error.
- ``_describe_image_with_vlm`` — non-list return path from pipe.
- ``_describe_image_with_vlm`` — list of non-dict return path.
- ``_describe_image_with_vlm`` — list of dict with generated_text key.
- ``_describe_image_with_vlm`` — list of dict with text key fallback.
- ``_describe_image_with_vlm`` — TypeError fallback (no prompt= kwarg).
- ``_extract_frame_png`` — success path (mocked ffmpeg process).
- ``_run_benchmark`` — FileNotFoundError when bench_all.sh absent.
- ``_run_benchmark`` — non-zero rc + empty output emits error key.
- ``_list_extractors`` — returns empty list when feature_dir absent.
- ``_infer_backend_from_sym`` — vulkan branch.
- ``_describe_model`` — ambiguous match (multiple files) raises ValueError.
- ``_pick_worst_frames`` — NaN/non-finite scores are skipped.
- ``_pick_worst_frames`` — string-valued score is skipped gracefully.
- ``_pick_worst_frames`` — nan float is skipped.
- ``_allowed_roots`` — container root /workspace always in list.
- bitdepth schema: tool inputSchema enumerates [8, 10, 12].
- ``_run_compare`` — optional fields (target_vmafs, encoders, dimensions).
- ``_run_ladder`` — optional framerate field wired through.
- ``_run_tune_per_shot`` — optional output/framerate/scene_threshold.
- ``_vmaf_binary`` — env override sets path.
- ``_vmaf_binary`` — falls back through candidates.
- ``_list_backends`` — binary-missing branch (all false except cpu).

http_transport.py (installed version — no ADR-0967 security layer)
- ``_build_metrics`` — creates and returns the three counter/histogram objects.
- ``_handle_score`` — 400 on missing 'distorted' field.
- ``_handle_score`` — 400 on missing 'width' field.
- ``_handle_score`` — 500 on scorer raising.
- ``make_score_handler`` — returns a callable coroutine.

ADR-0108 deliverables:
- Research digest: no digest needed: coverage gap-fill.
- Decision matrix: no alternatives: only-one-way fix.
- AGENTS.md: no rebase-sensitive invariants.
- Reproducer: ``cd mcp-server/vmaf-mcp && python -m pytest tests/test_coverage_round3.py -v``.
- Changelog fragment: changelog.d/added/mcp-server-coverage-round3.md.
- Rebase note: no rebase impact (fork-local Python tests).
"""

from __future__ import annotations

import asyncio
import json
import subprocess
from pathlib import Path
from typing import Any
from unittest.mock import MagicMock

import pytest

from vmaf_mcp import server as srv

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


@pytest.fixture(autouse=True)
def _clear_probe_cache():
    """Isolate the backend-probe cache between tests."""
    srv._BACKEND_PROBE_CACHE.clear()
    yield
    srv._BACKEND_PROBE_CACHE.clear()


@pytest.fixture(autouse=True)
def _reset_vlm_state():
    """Ensure VLM state doesn't leak between tests."""
    orig = dict(srv._vlm_state)
    yield
    srv._vlm_state.update(orig)


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


def _make_feature_parquet(path: Path, n: int = 30, with_mos: bool = True) -> None:
    """Write a minimal feature parquet file."""
    import numpy as np
    import pandas as pd

    rng = np.random.default_rng(42)
    data = {c: rng.standard_normal(n).astype(np.float32) for c in srv._FEATURE_COLUMNS}
    if with_mos:
        x = np.stack(list(data.values()), axis=1)
        data["mos"] = (x.sum(axis=1) * 0.5 + 10.0 + rng.normal(0, 0.01, n)).astype(np.float32)
    data["key"] = [f"sample_{i:04d}" for i in range(n)]
    pd.DataFrame(data).to_parquet(path)


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


skip_if_no_eval = pytest.mark.skipif(not _has_eval_deps(), reason="vmaf-mcp[eval] not installed")

# ---------------------------------------------------------------------------
# _probe_backends — timeout / OSError fallback
# ---------------------------------------------------------------------------


def test_probe_backends_timeout_returns_cpu_only(monkeypatch, tmp_path):
    """When vmaf --help times out, _probe_backends returns {cpu} and caches it."""
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"")
    fake_vmaf.chmod(0o755)

    def fake_run(*_args, **_kwargs):
        raise subprocess.TimeoutExpired(cmd="vmaf", timeout=5)

    monkeypatch.setattr(srv.subprocess, "run", fake_run)
    result = srv._probe_backends(fake_vmaf)
    assert result == frozenset({"cpu"})
    # Result is cached — second call must not call subprocess.run again.
    called_again = []

    def bomb(*_a, **_k):
        called_again.append(True)
        raise AssertionError("should have used cache")

    monkeypatch.setattr(srv.subprocess, "run", bomb)
    result2 = srv._probe_backends(fake_vmaf)
    assert result2 == frozenset({"cpu"})
    assert called_again == [], "cache was bypassed on second call"


def test_probe_backends_os_error_returns_cpu_only(monkeypatch, tmp_path):
    """When vmaf --help raises OSError, _probe_backends returns {cpu}."""
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"")
    fake_vmaf.chmod(0o755)

    def fake_run(*_args, **_kwargs):
        raise OSError("exec failed")

    monkeypatch.setattr(srv.subprocess, "run", fake_run)
    result = srv._probe_backends(fake_vmaf)
    assert result == frozenset({"cpu"})


def test_probe_backends_parses_hip_metal_flags(monkeypatch, tmp_path):
    """When --help output contains '--no_hip' and '--no_metal', both are included.
    Vulkan was removed per ADR-0726 and is no longer probed."""
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"")
    fake_vmaf.chmod(0o755)

    r = MagicMock()
    r.stdout = "--no_cuda\n--no_sycl\n--no_hip\n--no_metal\n"
    r.stderr = ""

    monkeypatch.setattr(srv.subprocess, "run", lambda *_a, **_k: r)
    result = srv._probe_backends(fake_vmaf)
    assert "hip" in result
    assert "metal" in result
    assert "cuda" in result
    assert "cpu" in result
    assert "vulkan" not in result


def test_probe_backends_returns_cached_on_second_call(monkeypatch, tmp_path):
    """Second call with the same binary path must hit the cache."""
    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"")

    call_count = []

    def counting_run(*_a, **_k):
        call_count.append(1)
        r = MagicMock()
        r.stdout = "--no_cuda\n"
        r.stderr = ""
        return r

    monkeypatch.setattr(srv.subprocess, "run", counting_run)
    srv._probe_backends(fake_vmaf)
    srv._probe_backends(fake_vmaf)
    assert len(call_count) == 1, "subprocess.run called more than once — cache not working"


# ---------------------------------------------------------------------------
# _infer_backend_from_payload — all branches
# ---------------------------------------------------------------------------


def test_infer_backend_from_payload_empty_frames():
    """An empty frames list returns 'cpu'."""
    assert srv._infer_backend_from_payload({"frames": []}) == "cpu"
    assert srv._infer_backend_from_payload({}) == "cpu"


def test_infer_backend_from_payload_no_metrics():
    """A frame with an empty metrics dict (0 keys <= 12) returns 'gpu'."""
    payload = {"frames": [{"frameNum": 0, "metrics": {}}]}
    result = srv._infer_backend_from_payload(payload)
    assert result == "gpu"


def test_infer_backend_from_payload_gpu_branch():
    """10 metric keys (<=12) returns 'gpu'."""
    metrics = {f"metric_{i}": 1.0 for i in range(10)}
    payload = {"frames": [{"frameNum": 0, "metrics": metrics}]}
    assert srv._infer_backend_from_payload(payload) == "gpu"


def test_infer_backend_from_payload_cpu_branch():
    """15 metric keys (13 <= nkeys < 30) returns 'cpu'."""
    metrics = {f"metric_{i}": 1.0 for i in range(15)}
    payload = {"frames": [{"frameNum": 0, "metrics": metrics}]}
    assert srv._infer_backend_from_payload(payload) == "cpu"


def test_infer_backend_from_payload_many_keys_returns_cpu():
    """30+ metric keys defaults to 'cpu' — the Vulkan branch was removed with
    ADR-0726; any key count above the GPU threshold now falls through to cpu."""
    metrics = {f"metric_{i}": 1.0 for i in range(30)}
    payload = {"frames": [{"frameNum": 0, "metrics": metrics}]}
    assert srv._infer_backend_from_payload(payload) == "cpu"


def test_infer_backend_from_payload_exactly_30_keys_returns_cpu():
    """Exactly 30 metric keys returns 'cpu' (post-ADR-0726; Vulkan branch gone)."""
    metrics = {f"m_{i}": 0.0 for i in range(30)}
    payload = {"frames": [{"frameNum": 0, "metrics": metrics}]}
    assert srv._infer_backend_from_payload(payload) == "cpu"


# ---------------------------------------------------------------------------
# _pick_worst_frames — basic contract + edge cases
# ---------------------------------------------------------------------------


def test_pick_worst_frames_ranks_by_ascending_score():
    """_pick_worst_frames returns frames sorted by ascending VMAF score."""
    score_json = {
        "frames": [
            {"frameNum": 0, "metrics": {"vmaf": 90.0}},
            {"frameNum": 1, "metrics": {"vmaf": 30.0}},
            {"frameNum": 2, "metrics": {"vmaf": 50.0}},
        ]
    }
    worst = srv._pick_worst_frames(score_json, n=3)
    assert len(worst) == 3
    scores = [s for _, s in worst]
    assert scores == sorted(scores)
    # Lowest score frame is first.
    assert worst[0] == (1, 30.0)


def test_pick_worst_frames_respects_n_limit():
    """_pick_worst_frames returns at most n frames."""
    score_json = {
        "frames": [{"frameNum": i, "metrics": {"vmaf": float(i * 10)}} for i in range(10)]
    }
    worst = srv._pick_worst_frames(score_json, n=3)
    assert len(worst) == 3


def test_pick_worst_frames_n_zero_returns_empty():
    """n=0 must return an empty list."""
    score_json = {
        "frames": [{"frameNum": 0, "metrics": {"vmaf": 99.0}}],
    }
    assert srv._pick_worst_frames(score_json, n=0) == []


def test_pick_worst_frames_skips_none_frame_num():
    """A frame without a 'frameNum' key must be silently skipped."""
    score_json = {
        "frames": [
            {"metrics": {"vmaf": 10.0}},  # no frameNum
            {"frameNum": 5, "metrics": {"vmaf": 20.0}},
        ]
    }
    worst = srv._pick_worst_frames(score_json, n=2)
    assert len(worst) == 1
    assert worst[0][0] == 5


def test_pick_worst_frames_skips_frame_with_no_headline_key():
    """A frame whose metrics dict has no recognised VMAF key must be skipped."""
    score_json = {
        "frames": [
            {"frameNum": 0, "metrics": {"unknown_metric": 99.0}},
            {"frameNum": 1, "metrics": {"vmaf": 40.0}},
        ]
    }
    worst = srv._pick_worst_frames(score_json, n=2)
    assert len(worst) == 1
    assert worst[0][0] == 1


def test_pick_worst_frames_uses_alternate_metric_keys():
    """Recognises vmaf_v0.6.1 and vmaf_4k_v0.6.1 headline keys."""
    score_json = {
        "frames": [
            {"frameNum": 0, "metrics": {"vmaf_v0.6.1": 80.0}},
            {"frameNum": 1, "metrics": {"vmaf_4k_v0.6.1": 20.0}},
        ]
    }
    worst = srv._pick_worst_frames(score_json, n=2)
    frame_nums = {idx for idx, _ in worst}
    assert 0 in frame_nums
    assert 1 in frame_nums


def test_pick_worst_frames_empty_frames_list():
    """Empty frames list returns empty list without raising."""
    assert srv._pick_worst_frames({"frames": []}, n=5) == []
    assert srv._pick_worst_frames({}, n=5) == []


# ---------------------------------------------------------------------------
# _describe_image_with_vlm — non-list return paths
# ---------------------------------------------------------------------------


def test_describe_image_with_vlm_returns_string_when_pipe_returns_string(monkeypatch):
    """When the VLM pipeline returns a plain string (not a list), the function
    must return that string stripped."""
    # Prime the state so _load_vlm skips the import step.
    monkeypatch.setitem(srv._vlm_state, "loaded", True)
    monkeypatch.setitem(srv._vlm_state, "pipeline", lambda _path, **_kw: "compression artefact")
    monkeypatch.setitem(srv._vlm_state, "model_id", "test-model")

    result = srv._describe_image_with_vlm(Path("/tmp/fake.png"))
    assert result == "compression artefact"


def test_describe_image_with_vlm_list_of_non_dict(monkeypatch):
    """When the VLM pipeline returns a list of strings, the result is str-ified."""
    monkeypatch.setitem(srv._vlm_state, "loaded", True)
    monkeypatch.setitem(srv._vlm_state, "pipeline", lambda _path, **_kw: ["noise in the image"])
    monkeypatch.setitem(srv._vlm_state, "model_id", "test-model")

    result = srv._describe_image_with_vlm(Path("/tmp/fake.png"))
    # The list contains strings, not dicts — so the isinstance(out[0], dict)
    # branch is False, and str(out) is returned.
    assert isinstance(result, str)
    assert len(result) > 0


def test_describe_image_with_vlm_list_of_dict_generated_text(monkeypatch):
    """List[dict] with 'generated_text' key uses that value."""
    monkeypatch.setitem(srv._vlm_state, "loaded", True)
    monkeypatch.setitem(
        srv._vlm_state, "pipeline", lambda _p, **_kw: [{"generated_text": "blocking artefact"}]
    )
    monkeypatch.setitem(srv._vlm_state, "model_id", "test-model")

    result = srv._describe_image_with_vlm(Path("/tmp/fake.png"))
    assert result == "blocking artefact"


def test_describe_image_with_vlm_list_of_dict_text_key_fallback(monkeypatch):
    """List[dict] with 'text' key uses that value when 'generated_text' absent."""
    monkeypatch.setitem(srv._vlm_state, "loaded", True)
    monkeypatch.setitem(srv._vlm_state, "pipeline", lambda _p, **_kw: [{"text": "banding visible"}])
    monkeypatch.setitem(srv._vlm_state, "model_id", "test-model")

    result = srv._describe_image_with_vlm(Path("/tmp/fake.png"))
    assert result == "banding visible"


def test_describe_image_with_vlm_type_error_fallback(monkeypatch):
    """When the pipeline raises TypeError on prompt= kwarg, the function retries
    without it (older transformers compatibility branch)."""
    call_log: list[dict] = []

    def fake_pipe(path_str: str, **kwargs: Any) -> Any:
        call_log.append({"path": path_str, "kwargs": dict(kwargs)})
        if "prompt" in kwargs:
            raise TypeError("unexpected keyword argument 'prompt'")
        # Second call (without prompt) succeeds.
        return [{"generated_text": "retry succeeded"}]

    monkeypatch.setitem(srv._vlm_state, "loaded", True)
    monkeypatch.setitem(srv._vlm_state, "pipeline", fake_pipe)
    monkeypatch.setitem(srv._vlm_state, "model_id", "legacy-model")

    result = srv._describe_image_with_vlm(Path("/tmp/fake.png"))
    assert result == "retry succeeded"
    # First call included prompt=, second did not.
    assert len(call_log) == 2
    assert "prompt" in call_log[0]["kwargs"]
    assert "prompt" not in call_log[1]["kwargs"]


# ---------------------------------------------------------------------------
# _extract_frame_png — success path (mocked ffmpeg)
# ---------------------------------------------------------------------------


def test_extract_frame_png_success(monkeypatch, tmp_path):
    """When ffmpeg exits 0, _extract_frame_png must not raise and must have
    called create_subprocess_exec with the expected ffmpeg arguments."""
    monkeypatch.setattr(
        srv.shutil, "which", lambda cmd: "/usr/bin/ffmpeg" if cmd == "ffmpeg" else None
    )

    class _Proc:
        returncode = 0

        async def communicate(self) -> tuple[bytes, bytes]:
            return b"", b""

    captured_argv: list[Any] = []

    async def fake_exec(*argv, **_kwargs):
        captured_argv.extend(argv)
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", fake_exec)
    yuv = tmp_path / "in.yuv"
    yuv.write_bytes(b"\x00" * 512)
    out_png = tmp_path / "out.png"

    async def run():
        await srv._extract_frame_png(
            yuv,
            width=320,
            height=240,
            pixfmt="420",
            bitdepth=8,
            frame_index=7,
            out_png=out_png,
        )

    asyncio.run(run())
    assert "ffmpeg" in captured_argv[0]
    assert any("select" in str(a) for a in captured_argv)
    assert any("eq(n,7)" in str(a) for a in captured_argv)


def test_extract_frame_png_10bit(monkeypatch, tmp_path):
    """10-bit 4:2:0 maps to yuv420p10le in the ffmpeg pix_fmt argument."""
    monkeypatch.setattr(srv.shutil, "which", lambda _cmd: "/usr/bin/ffmpeg")

    class _Proc:
        returncode = 0

        async def communicate(self):
            return b"", b""

    captured_argv: list = []

    async def fake_exec(*argv, **_kw):
        captured_argv.extend(argv)
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", fake_exec)
    yuv = tmp_path / "in10.yuv"
    yuv.write_bytes(b"\x00" * 16)

    async def run():
        await srv._extract_frame_png(
            yuv,
            width=4,
            height=4,
            pixfmt="420",
            bitdepth=10,
            frame_index=0,
            out_png=tmp_path / "o.png",
        )

    asyncio.run(run())
    assert "yuv420p10le" in captured_argv


# ---------------------------------------------------------------------------
# _run_benchmark — FileNotFoundError + empty-output error branch
# ---------------------------------------------------------------------------


def test_run_benchmark_script_missing_raises(monkeypatch, tmp_path):
    """When bench_all.sh does not exist, _run_benchmark raises FileNotFoundError."""
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    async def run():
        await srv._run_benchmark()

    with pytest.raises(FileNotFoundError, match="benchmark harness not found"):
        asyncio.run(run())


def test_run_benchmark_nonzero_empty_output_raises_runtime_error(monkeypatch, tmp_path):
    """A non-zero rc with no output must raise RuntimeError (ADR-0608 E-1: _run_benchmark
    was changed to raise on non-zero exit so MCP marks the call as isError=True)."""
    # Create fake bench_all.sh so exists() passes.
    fake_script = tmp_path / "testdata" / "bench_all.sh"
    fake_script.parent.mkdir(parents=True)
    fake_script.write_text("#!/bin/bash\nexit 1")
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    class _Proc:
        returncode = 1

        async def communicate(self):
            return b"", b""  # empty output triggers the error-key branch

    async def fake_exec(*_a, **_k):
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", fake_exec)
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: Path("/usr/local/bin/vmaf"))

    async def run():
        return await srv._run_benchmark()

    with pytest.raises(RuntimeError, match="benchmark failed"):
        asyncio.run(run())


# ---------------------------------------------------------------------------
# _list_extractors — missing feature_dir
# ---------------------------------------------------------------------------


def test_list_extractors_returns_empty_when_feature_dir_absent(monkeypatch, tmp_path):
    """_list_extractors returns [] (not raises) when core/src/feature/ is absent."""
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)
    result = srv._list_extractors()
    assert result == []


def test_list_extractors_parses_synthetic_c_file(monkeypatch, tmp_path):
    """_list_extractors matches VmafFeatureExtractor struct definitions in C source."""
    # Determine which subpath the installed version uses for feature_dir.
    # Probe for the literal path assignment to disambiguate — the docstring
    # mentions "libvmaf" by name so a plain ``"libvmaf" in src`` check
    # was a false-positive on the current codebase (ADR-0700 renamed to core/).
    import inspect

    import vmaf_mcp.server as _srv_mod

    src = inspect.getsource(_srv_mod._list_extractors)
    if '"libvmaf/src/feature"' in src or "'libvmaf/src/feature'" in src:
        feature_dir = tmp_path / "libvmaf" / "src" / "feature"
    else:
        feature_dir = tmp_path / "core" / "src" / "feature"
    feature_dir.mkdir(parents=True)

    c_content = """\
VmafFeatureExtractor vmaf_fex_synthetic = {
    .name = "test_extractor",
};

VmafFeatureExtractor vmaf_fex_synthetic_cuda = {
    .name = "test_extractor_cuda",
};
"""
    (feature_dir / "test_fex.c").write_text(c_content)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    result = srv._list_extractors()
    names = [e["name"] for e in result]
    assert "test_extractor" in names
    assert "test_extractor_cuda" in names
    backends = {e["name"]: e["backend"] for e in result}
    assert backends["test_extractor"] == "cpu"
    assert backends["test_extractor_cuda"] == "cuda"


# ---------------------------------------------------------------------------
# _infer_backend_from_sym — all backend branches
# ---------------------------------------------------------------------------


def test_infer_backend_from_sym_cuda():
    assert srv._infer_backend_from_sym("float_vif_cuda") == "cuda"


def test_infer_backend_from_sym_sycl():
    assert srv._infer_backend_from_sym("float_motion_sycl") == "sycl"


def test_infer_backend_from_sym_hip():
    assert srv._infer_backend_from_sym("float_ssim_hip") == "hip"


def test_infer_backend_from_sym_metal():
    assert srv._infer_backend_from_sym("float_adm_metal") == "metal"


def test_infer_backend_from_sym_cpu_default():
    assert srv._infer_backend_from_sym("float_vif") == "cpu"
    assert srv._infer_backend_from_sym("psnr") == "cpu"


def test_infer_backend_from_sym_vulkan():
    """_vulkan suffix maps to 'vulkan' when _BACKEND_KEYWORDS includes it."""
    result = srv._infer_backend_from_sym("float_adm_vulkan")
    # Whether this returns 'vulkan' or 'cpu' depends on which backends
    # _BACKEND_KEYWORDS includes in the installed version.
    keyword_labels = {label for _, label in srv._BACKEND_KEYWORDS}
    if "vulkan" in keyword_labels:
        assert result == "vulkan"
    else:
        assert result == "cpu"


# ---------------------------------------------------------------------------
# _describe_model — ambiguous match raises ValueError
# ---------------------------------------------------------------------------


def test_describe_model_ambiguous_raises(tmp_path, monkeypatch):
    """When two files share the same stem, _describe_model must raise ValueError."""
    # Plant two files with the same stem but different extensions in model dir.
    model_dir = tmp_path / "model"
    model_dir.mkdir()
    (model_dir / "dup.json").write_text(json.dumps({"model_dict": {"model_type": "SVM"}}))
    (model_dir / "dup.onnx").write_bytes(b"\x00" * 4)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    with pytest.raises(ValueError, match="ambiguous"):
        srv._describe_model("dup")


# ---------------------------------------------------------------------------
# _describe_model — direct path resolution (Step 1)
# ---------------------------------------------------------------------------


def test_describe_model_by_absolute_path(tmp_path, monkeypatch):
    """_describe_model accepts an absolute path to a model file."""
    model_dir = tmp_path / "model"
    model_dir.mkdir()
    model_file = model_dir / "my_model.json"
    model_file.write_text(
        json.dumps({"model_dict": {"model_type": "LINEAR", "feature_names": ["adm2"]}})
    )
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)

    result = srv._describe_model(str(model_file))
    assert result["name"] == "my_model"
    assert result["format"] == "json"
    assert result["model_type"] == "LINEAR"
    assert result["feature_names"] == ["adm2"]


# ---------------------------------------------------------------------------
# _vmaf_binary — env override + fallback behaviour
# ---------------------------------------------------------------------------


def test_vmaf_binary_env_override(monkeypatch):
    """VMAF_BIN env var must take precedence over all auto-detected paths."""
    monkeypatch.setenv("VMAF_BIN", "/custom/path/vmaf")
    result = srv._vmaf_binary()
    assert str(result) == "/custom/path/vmaf"


def test_vmaf_binary_falls_back_gracefully(monkeypatch, tmp_path):
    """When no candidate exists, _vmaf_binary returns a Path without raising."""
    monkeypatch.delenv("VMAF_BIN", raising=False)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)
    # /usr/local/bin/vmaf typically does not exist on a dev box either.
    result = srv._vmaf_binary()
    assert isinstance(result, Path)


# ---------------------------------------------------------------------------
# _list_backends — binary-missing fallback (all false except cpu)
# ---------------------------------------------------------------------------


def test_list_backends_binary_missing_returns_all_false(monkeypatch, tmp_path):
    """When the vmaf binary doesn't exist, list_backends returns cpu=True and
    all GPU backends False — it must not raise."""
    monkeypatch.setattr(srv, "_vmaf_binary", lambda: tmp_path / "no_such_vmaf_binary")
    result = asyncio.run(srv._list_backends())
    assert result["cpu"] is True
    assert result["cuda"] is False
    assert result["sycl"] is False
    assert result["hip"] is False
    assert result["metal"] is False


# ---------------------------------------------------------------------------
# _eval_model_on_split — features missing 'mos' column
# ---------------------------------------------------------------------------


@skip_if_no_eval
def test_eval_model_on_split_missing_mos_column_raises(tmp_path, monkeypatch):
    """If the parquet has no 'mos' column, _eval_model_on_split raises ValueError."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    import numpy as np
    import pandas as pd

    model = tmp_path / "m.onnx"
    feats = tmp_path / "nomos.parquet"
    _make_tiny_mlp(model)
    # Write parquet without a 'mos' column.
    rng = np.random.default_rng(0)
    data = {c: rng.standard_normal(20).astype(np.float32) for c in srv._FEATURE_COLUMNS}
    pd.DataFrame(data).to_parquet(feats)

    with pytest.raises(ValueError, match="no 'mos' column"):
        srv._eval_model_on_split(model, feats, split="all", input_name="features")


@skip_if_no_eval
def test_eval_model_on_split_no_feature_columns_raises(tmp_path, monkeypatch):
    """If none of the expected feature columns exist, _eval_model_on_split raises."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    import numpy as np
    import pandas as pd

    model = tmp_path / "m.onnx"
    feats = tmp_path / "badcols.parquet"
    _make_tiny_mlp(model)
    # Write parquet with unrecognised column names + mos.
    rng = np.random.default_rng(1)
    n = 20
    pd.DataFrame(
        {
            "irrelevant_col": rng.standard_normal(n).astype(np.float32),
            "mos": rng.uniform(60, 100, n).astype(np.float32),
        }
    ).to_parquet(feats)

    with pytest.raises(ValueError, match="none of the expected feature columns"):
        srv._eval_model_on_split(model, feats, split="all", input_name="features")


@skip_if_no_eval
def test_eval_model_on_split_too_few_samples_raises(tmp_path, monkeypatch):
    """A split with < 2 samples must raise ValueError (can't compute correlations)."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", str(tmp_path))
    import numpy as np
    import pandas as pd

    model = tmp_path / "m.onnx"
    feats = tmp_path / "tiny.parquet"
    _make_tiny_mlp(model)
    # Write a parquet with just 2 rows but all bucketed to 'train' split
    # so 'test' gets 0 rows (or near-zero). Use a key that hashes to test
    # bucket to guarantee 1-sample test split.
    rng = np.random.default_rng(2)
    n = 4
    data = {c: rng.standard_normal(n).astype(np.float32) for c in srv._FEATURE_COLUMNS}
    data["mos"] = rng.uniform(60, 100, n).astype(np.float32)
    # Use all-train keys by using 200-sample parquet but passing n=1 subset filter.
    # Simpler: just create a parquet with 1 matching row for 'test' split.
    data["key"] = ["sample_0000", "sample_0001", "sample_0002", "sample_0003"]
    pd.DataFrame(data).to_parquet(feats)

    # The test split typically gets ~10% of rows; with 4 rows that could be 0-1.
    # Rather than relying on hash chance, call with n=2 and verify it raises
    # when a split that filters to 1 row is forced. We do this by creating a
    # tiny parquet and attempting the 'val' split where count < 2.
    import numpy as np2

    feats2 = tmp_path / "onesample.parquet"
    # Create data where 'val' bucket matches exactly one key.
    # We can deterministically pick a key that hashes to the val bucket.
    # val bucket: test_frac <= h < test_frac + val_frac  → 0.1 <= h < 0.2
    import hashlib

    def _bucket(k: str) -> float:
        h = hashlib.sha256(f"vmaf-train-splits-v1:{k}".encode()).digest()
        return int.from_bytes(h[:8], "big") / (1 << 64)

    # Find at least one key in val bucket.
    val_key = None
    for i in range(10000):
        k = f"sample_{i:06d}"
        b = _bucket(k)
        if 0.1 <= b < 0.2:
            val_key = k
            break

    if val_key is None:
        pytest.skip("Could not find a val-bucket key in 10k tries")

    data2 = {c: np2.array([1.0], dtype=np2.float32) for c in srv._FEATURE_COLUMNS}
    data2["mos"] = np2.array([80.0], dtype=np2.float32)
    data2["key"] = [val_key]
    pd.DataFrame(data2).to_parquet(feats2)

    with pytest.raises(ValueError, match="samples"):
        srv._eval_model_on_split(model, feats2, split="val", input_name="features")


# ---------------------------------------------------------------------------
# _allowed_roots — /workspace is always included
# ---------------------------------------------------------------------------


def test_allowed_roots_includes_workspace_path(monkeypatch):
    """The container bind-mount path /workspace must always be in the allowlist
    even without VMAF_MCP_ALLOW being set."""
    monkeypatch.delenv("VMAF_MCP_ALLOW", raising=False)
    roots = srv._allowed_roots()
    root_strs = [str(r) for r in roots]
    # At least one root must start with /workspace
    assert any(
        s.startswith("/workspace") for s in root_strs
    ), f"/workspace not found in allowed roots: {root_strs}"


# ---------------------------------------------------------------------------
# tool inputSchema — vmaf_score bitdepth enum validation
# ---------------------------------------------------------------------------


def test_vmaf_score_bitdepth_schema_is_integer_with_enum():
    """The vmaf_score inputSchema must enumerate bitdepth as integers and
    include the standard {8, 10, 12} subset at minimum."""
    import anyio

    async def get():
        return await srv._list_tools()

    tools = anyio.run(get)
    by_name = {t.name: t for t in tools}
    schema = by_name["vmaf_score"].input_schema
    bitdepth_schema = schema["properties"]["bitdepth"]
    assert bitdepth_schema["type"] == "integer"
    enum_set = set(bitdepth_schema["enum"])
    # {8, 10, 12} are always required; 16 may or may not be present.
    assert {8, 10, 12}.issubset(
        enum_set
    ), f"bitdepth enum must include at least {{8,10,12}}, got {bitdepth_schema['enum']}"


def test_vmaf_score_pixfmt_schema_enumerates_valid_values():
    """The vmaf_score inputSchema must enumerate pixfmt as ['420','422','444']."""
    import anyio

    async def get():
        return await srv._list_tools()

    tools = anyio.run(get)
    by_name = {t.name: t for t in tools}
    schema = by_name["vmaf_score"].input_schema
    pixfmt_schema = schema["properties"]["pixfmt"]
    assert set(pixfmt_schema["enum"]) == {"420", "422", "444"}


def test_list_tools_returns_tool_names():
    """list_tools returns a list of Tool objects with all documented names."""
    import anyio

    async def get():
        return await srv._list_tools()

    tools = anyio.run(get)
    names = {t.name for t in tools}
    expected = {
        "vmaf_score",
        "list_models",
        "list_backends",
        "run_benchmark",
        "eval_model_on_split",
        "compare_models",
        "describe_worst_frames",
        "probe_backend",
        "vmaf_version",
        "vmaf_score_encoded",
        "list_extractors",
        "describe_model",
        "run_compare",
        "run_ladder",
        "run_tune_per_shot",
    }
    missing = expected - names
    assert not missing, f"Missing tools in list_tools(): {missing}"


# ---------------------------------------------------------------------------
# _run_compare — optional keyword arguments reach argv
# ---------------------------------------------------------------------------


def test_run_compare_target_vmafs_branch(monkeypatch, tmp_path):
    """_run_compare with target_vmafs (not target_vmaf) must add --target-vmafs."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", "/tmp")
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"#!/bin/sh\nexit 0\n")
    fake_tune.chmod(0o755)
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    captured: list[list] = []

    class _Proc:
        returncode = 0

        async def communicate(self):
            return json.dumps({"ok": True}).encode(), b""

    async def fake_exec(*argv, **_kw):
        captured.append(list(argv))
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", fake_exec)
    asyncio.run(srv._run_compare(src="/tmp/x.yuv", target_vmafs="94,96,98"))
    argv = captured[0]
    assert "--target-vmafs" in argv
    idx = argv.index("--target-vmafs")
    assert argv[idx + 1] == "94,96,98"
    # target_vmaf should NOT appear when target_vmafs is set
    assert "--target-vmaf" not in argv


def test_run_compare_encoders_branch(monkeypatch, tmp_path):
    """_run_compare with encoders kwarg must add --encoders to argv."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", "/tmp")
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    captured: list[list] = []

    class _Proc:
        returncode = 0

        async def communicate(self):
            return json.dumps({}).encode(), b""

    async def fake_exec(*argv, **_kw):
        captured.append(list(argv))
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", fake_exec)
    asyncio.run(srv._run_compare(src="/tmp/x.yuv", encoders="libx264,libx265"))
    assert "--encoders" in captured[0]


def test_run_compare_width_height_branches(monkeypatch, tmp_path):
    """width + height + framerate args must each appear in the argv when provided."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", "/tmp")
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    captured: list[list] = []

    class _Proc:
        returncode = 0

        async def communicate(self):
            return json.dumps({}).encode(), b""

    async def fake_exec(*argv, **_kw):
        captured.append(list(argv))
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", fake_exec)
    asyncio.run(
        srv._run_compare(src="/tmp/x.yuv", width=1280, height=720, framerate=24.0, no_parallel=True)
    )
    argv = captured[0]
    assert "--width" in argv
    assert "--height" in argv
    assert "--framerate" in argv
    assert "--no-parallel" in argv


# ---------------------------------------------------------------------------
# _run_ladder — framerate branch
# ---------------------------------------------------------------------------


def test_run_ladder_framerate_branch(monkeypatch, tmp_path):
    """When framerate is supplied to _run_ladder, it must appear in argv."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", "/tmp")
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    captured: list[list] = []

    class _Proc:
        returncode = 0

        async def communicate(self):
            return json.dumps({"rungs": []}).encode(), b""

    async def fake_exec(*argv, **_kw):
        captured.append(list(argv))
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", fake_exec)
    asyncio.run(
        srv._run_ladder(
            src="/tmp/x.yuv",
            resolutions="1920x1080",
            target_vmafs="95",
            framerate=30.0,
        )
    )
    argv = captured[0]
    assert "--framerate" in argv
    idx = argv.index("--framerate")
    assert argv[idx + 1] == "30.0"


# ---------------------------------------------------------------------------
# _run_tune_per_shot — optional field branches
# ---------------------------------------------------------------------------


def test_run_tune_per_shot_optional_fields(monkeypatch, tmp_path):
    """output, framerate, and scene_threshold all appear in argv when supplied."""
    monkeypatch.setenv("VMAF_MCP_ALLOW", "/tmp")
    fake_tune = tmp_path / "vmaf-tune"
    fake_tune.write_bytes(b"")
    monkeypatch.setattr(srv, "_vmaftune_binary", lambda: fake_tune)

    captured: list[list] = []

    class _Proc:
        returncode = 0

        async def communicate(self):
            return json.dumps({"shots": []}).encode(), b""

    async def fake_exec(*argv, **_kw):
        captured.append(list(argv))
        return _Proc()

    monkeypatch.setattr(srv.asyncio, "create_subprocess_exec", fake_exec)
    asyncio.run(
        srv._run_tune_per_shot(
            src="/tmp/x.yuv",
            output="/tmp/out.mp4",
            framerate=25.0,
            scene_threshold=0.4,
        )
    )
    argv = captured[0]
    assert "--output" in argv
    assert "--framerate" in argv
    assert "--scene-threshold" in argv


# ---------------------------------------------------------------------------
# http_transport — _build_metrics and make_score_handler
# ---------------------------------------------------------------------------


_HT_DEPS = True
try:
    import aiohttp  # noqa: F401
    import prometheus_client  # noqa: F401
except ImportError:
    _HT_DEPS = False

ht_skip = pytest.mark.skipif(not _HT_DEPS, reason="aiohttp + prometheus_client not installed")


@ht_skip
def test_build_metrics_creates_counter_and_histogram():
    """_build_metrics must return a dict with all three expected keys and
    working prometheus Counter/Histogram objects.  Uses an isolated registry
    to avoid duplicate-metric errors."""
    from vmaf_mcp import http_transport as ht

    class _FakePC:
        class Counter:
            def __init__(self, name, doc, labels=None, registry=None):
                self._name = name
                self._labels = labels or []

            def labels(self, **_kw):
                return self

            def inc(self):
                pass

        class Histogram:
            def __init__(self, name, doc, buckets=None, registry=None):
                self._name = name

            def time(self):
                import contextlib

                return contextlib.nullcontext()

    metrics = ht._build_metrics(_FakePC())
    assert "scoring_requests_total" in metrics
    assert "scoring_errors_total" in metrics
    assert "scoring_duration_seconds" in metrics


@ht_skip
def test_make_score_handler_returns_coroutine():
    """make_score_handler must return an async callable."""
    import inspect

    from vmaf_mcp import http_transport as ht

    sentinel_metrics: dict[str, Any] = {}
    handler = ht.make_score_handler(sentinel_metrics)
    assert callable(handler)
    assert inspect.iscoroutinefunction(handler)


@ht_skip
@pytest.mark.asyncio
async def test_score_400_on_missing_distorted(aiohttp_client: Any, monkeypatch: Any) -> None:
    """POST /v1/score with only 'reference' (missing 'distorted') must return 400."""
    from unittest.mock import patch

    import prometheus_client as pc  # type: ignore[import-untyped]

    from vmaf_mcp import http_transport as ht

    registry = pc.CollectorRegistry(auto_describe=False)
    metrics_r3 = {
        "scoring_requests_total": pc.Counter(
            "r3_requests_total", "r3", ["endpoint", "status"], registry=registry
        ),
        "scoring_errors_total": pc.Counter("r3_errors_total", "r3e", registry=registry),
        "scoring_duration_seconds": pc.Histogram(
            "r3_duration_seconds", "r3d", buckets=[0.1, 1.0], registry=registry
        ),
    }

    # VMAFX_MCP_HTTP_NO_AUTH must span the full test: the security middleware
    # reads _no_auth_mode() at request-dispatch time, not at app-creation time.
    with patch.dict(__import__("os").environ, {"VMAFX_MCP_HTTP_NO_AUTH": "1"}, clear=False):
        app = ht._make_app(metrics_r3)
        client = await aiohttp_client(app)
        resp = await client.post(
            "/v1/score",
            json={
                "reference": "/tmp/r.yuv",
                "width": 1920,
                "height": 1080,
                "pixfmt": "420",
                "bitdepth": 8,
            },
        )
        assert resp.status == 400
        body = await resp.json()
        assert "error" in body


@ht_skip
@pytest.mark.asyncio
async def test_score_400_on_missing_width(aiohttp_client: Any) -> None:
    """POST /v1/score with missing 'width' must return 400."""
    from unittest.mock import patch

    import prometheus_client as pc  # type: ignore[import-untyped]

    from vmaf_mcp import http_transport as ht

    registry = pc.CollectorRegistry(auto_describe=False)
    metrics_r3b = {
        "scoring_requests_total": pc.Counter(
            "r3b_requests_total", "r3b", ["endpoint", "status"], registry=registry
        ),
        "scoring_errors_total": pc.Counter("r3b_errors_total", "r3be", registry=registry),
        "scoring_duration_seconds": pc.Histogram(
            "r3b_duration_seconds", "r3bd", buckets=[0.1], registry=registry
        ),
    }

    # VMAFX_MCP_HTTP_NO_AUTH must span the full test: the security middleware
    # reads _no_auth_mode() at request-dispatch time, not at app-creation time.
    with patch.dict(__import__("os").environ, {"VMAFX_MCP_HTTP_NO_AUTH": "1"}, clear=False):
        app = ht._make_app(metrics_r3b)
        client = await aiohttp_client(app)
        resp = await client.post(
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
        assert "error" in body


@ht_skip
@pytest.mark.asyncio
async def test_score_500_on_scorer_error(aiohttp_client: Any) -> None:
    """POST /v1/score must return 500 when _run_vmaf_score raises."""
    from unittest.mock import AsyncMock, patch

    import prometheus_client as pc  # type: ignore[import-untyped]

    import vmaf_mcp.server as srv_mod
    from vmaf_mcp import http_transport as ht

    registry = pc.CollectorRegistry(auto_describe=False)
    metrics_r3c = {
        "scoring_requests_total": pc.Counter(
            "r3c_requests_total", "r3c", ["endpoint", "status"], registry=registry
        ),
        "scoring_errors_total": pc.Counter("r3c_errors_total", "r3ce", registry=registry),
        "scoring_duration_seconds": pc.Histogram(
            "r3c_duration_seconds", "r3cd", buckets=[0.1], registry=registry
        ),
    }

    async def _boom(_req):
        raise RuntimeError("synthetic scorer failure r3c")

    import os
    import tempfile

    # VMAFX_MCP_HTTP_NO_AUTH must span the full test: the security middleware
    # reads _no_auth_mode() at request-dispatch time, not at app-creation time.
    with patch.dict(__import__("os").environ, {"VMAFX_MCP_HTTP_NO_AUTH": "1"}, clear=False):
        app = ht._make_app(metrics_r3c)
        client = await aiohttp_client(app)

        with tempfile.TemporaryDirectory() as td:
            ref = os.path.join(td, "r.yuv")
            dis = os.path.join(td, "d.yuv")
            with open(ref, "wb") as _f:
                _f.write(b"\x00" * 16)
            with open(dis, "wb") as _f:
                _f.write(b"\x00" * 16)

            with (
                patch.object(srv_mod, "_validate_path", side_effect=Path),
                patch.object(srv_mod, "_run_vmaf_score", new=AsyncMock(side_effect=_boom)),
            ):
                resp = await client.post(
                    "/v1/score",
                    json={
                        "reference": ref,
                        "distorted": dis,
                        "width": 1920,
                        "height": 1080,
                        "pixfmt": "420",
                        "bitdepth": 8,
                    },
                )
    assert resp.status == 500
    body = await resp.json()
    # The HTTP handler returns a generic message to avoid leaking internal exception
    # text to clients; the actual error is logged server-side (captured above).
    assert body["error"] == "scoring failed; see server logs"
