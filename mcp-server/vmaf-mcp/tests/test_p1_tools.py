# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the MCP P1 surface additions (ADR-0608).

Covers:
- list_extractors   — C source parse, backend tag, dedup
- describe_model    — Path.stem bug fix, JSON metadata, unknown-name error
- run_compare       — subprocess mock, progress token plumbing
- run_ladder        — subprocess mock, JSON + HLS formats
- run_tune_per_shot — subprocess mock, error handling
- run_benchmark     — progress token plumbing (patched function signature)
- _send_progress    — no-op outside request context
- tool registration — all six new tools appear in list_tools()
"""

from __future__ import annotations

import json
from pathlib import Path
from unittest.mock import AsyncMock, patch

import anyio
import pytest

from vmaf_mcp import server as srv

REPO = Path(__file__).resolve().parents[3]
WORKTREE = Path(__file__).resolve().parents[4]


# ---------------------------------------------------------------------------
# list_extractors
# ---------------------------------------------------------------------------


def test_list_extractors_returns_list():
    """list_extractors must return a non-empty list when the C source tree
    is present (it always is in this repo)."""
    extractors = srv._list_extractors()
    assert isinstance(extractors, list)
    assert len(extractors) > 0, "Expected at least one extractor in the C source tree"


def test_list_extractors_fields():
    """Every extractor dict must have 'name', 'backend', and 'source' keys."""
    extractors = srv._list_extractors()
    for ex in extractors:
        assert "name" in ex, f"Missing 'name' in {ex}"
        assert "backend" in ex, f"Missing 'backend' in {ex}"
        assert "source" in ex, f"Missing 'source' in {ex}"


def test_list_extractors_known_names():
    """At minimum the canonical CPU extractors must be present.

    The Vulkan twins were dropped wholesale by ADR-0726 (2026-05-28).
    """
    names = {ex["name"] for ex in srv._list_extractors()}
    for expected in ("float_vif", "psnr", "ssim", "vif"):
        assert expected in names, f"Expected extractor {expected!r} not found; got {sorted(names)}"


def test_list_extractors_backend_tags():
    """Backend tags must only be known labels (post-ADR-0726)."""
    valid = {"cpu", "cuda", "sycl", "hip", "metal"}
    for ex in srv._list_extractors():
        assert (
            ex["backend"] in valid
        ), f"Unexpected backend tag {ex['backend']!r} for extractor {ex['name']!r}"


def test_list_extractors_cpu_backend_for_cpu_extractors():
    """float_vif / ssim are CPU-only and must carry backend='cpu'."""
    by_name = {ex["name"]: ex for ex in srv._list_extractors()}
    for cpu_name in ("float_vif", "ssim"):
        if cpu_name in by_name:
            assert by_name[cpu_name]["backend"] == "cpu", (
                f"Extractor {cpu_name!r} should be tagged cpu, "
                f"got {by_name[cpu_name]['backend']!r}"
            )


def test_list_extractors_gpu_backend_tags():
    """CUDA / SYCL / HIP twins carry the correct backend tag (post-ADR-0726)."""
    by_name = {ex["name"]: ex for ex in srv._list_extractors()}
    # float_motion_cuda is registered in the CUDA C source.
    if "float_motion_cuda" in by_name:
        assert by_name["float_motion_cuda"]["backend"] == "cuda"
    if "float_ssim_hip" in by_name:
        assert by_name["float_ssim_hip"]["backend"] == "hip"


def test_list_extractors_no_duplicates():
    """The (name, source) pair must be unique — no double-registration."""
    extractors = srv._list_extractors()
    seen: set[tuple[str, str]] = set()
    for ex in extractors:
        key = (ex["name"], ex["source"])
        assert key not in seen, f"Duplicate extractor entry: {key}"
        seen.add(key)


# ---------------------------------------------------------------------------
# _strip_model_ext — Path.stem bug regression
# ---------------------------------------------------------------------------


def test_strip_model_ext_json():
    assert srv._strip_model_ext("vmaf_v0.6.1.json") == "vmaf_v0.6.1"


def test_strip_model_ext_onnx():
    assert srv._strip_model_ext("model_v2.onnx") == "model_v2"


def test_strip_model_ext_pkl():
    assert srv._strip_model_ext("nflxall_libsvmnusvr.pkl") == "nflxall_libsvmnusvr"


def test_strip_model_ext_no_extension():
    # No recognised extension — return unchanged (the key bug path).
    result = srv._strip_model_ext("vmaf_v0.6.1")
    # Must not strip the ".1" portion the way Path.stem would.
    assert (
        result == "vmaf_v0.6.1"
    ), f"Path.stem bug: _strip_model_ext stripped '.1' from 'vmaf_v0.6.1'; got {result!r}"


def test_strip_model_ext_unknown_extension():
    # Unknown extension treated as part of the name.
    result = srv._strip_model_ext("vmaf_v0.6.1.bin")
    assert result == "vmaf_v0.6.1.bin"


# ---------------------------------------------------------------------------
# describe_model
# ---------------------------------------------------------------------------


def test_describe_model_by_stem():
    """describe_model('vmaf_v0.6.1') must resolve to vmaf_v0.6.1.json without
    the Path.stem bug trimming it to vmaf_v0.6."""
    result = srv._describe_model("vmaf_v0.6.1")
    assert result["name"] == "vmaf_v0.6.1"
    assert result["format"] == "json"
    assert "vmaf_v0.6.1.json" in result["path"]


def test_describe_model_json_metadata():
    """JSON models expose model_type and feature_names."""
    result = srv._describe_model("vmaf_v0.6.1")
    # vmaf_v0.6.1.json uses LIBSVMNUSVR.
    assert result["model_type"] is not None
    assert isinstance(result["feature_names"], list)
    assert len(result["feature_names"]) > 0


def test_describe_model_by_full_filename():
    result = srv._describe_model("vmaf_v0.6.1.json")
    assert result["name"] == "vmaf_v0.6.1"
    assert result["format"] == "json"


def test_describe_model_unknown_raises():
    with pytest.raises(ValueError, match="not found"):
        srv._describe_model("this_model_does_not_exist_at_all")


def test_describe_model_onnx_no_metadata(tmp_path, monkeypatch):
    """ONNX models return model_type=None and feature_names=None."""
    # Redirect _repo_root to tmp_path so the test is decoupled from the
    # physical install layout (the installed package's _repo_root() resolves
    # to its own worktree root, which differs from REPO).
    model_dir = tmp_path / "model"
    model_dir.mkdir()
    fake_model = model_dir / "_mcp_test_fake.onnx"
    fake_model.write_bytes(b"\x00" * 4)
    monkeypatch.setattr(srv, "_repo_root", lambda: tmp_path)
    result = srv._describe_model("_mcp_test_fake")
    assert result["format"] == "onnx"
    assert result["model_type"] is None
    assert result["feature_names"] is None


def test_describe_model_size_bytes():
    result = srv._describe_model("vmaf_v0.6.1")
    assert result["size_bytes"] > 0


# ---------------------------------------------------------------------------
# run_benchmark — progress token (signature changed to accept progress_token)
# ---------------------------------------------------------------------------


def test_run_benchmark_progress_token_plumbing():
    """_run_benchmark now accepts an optional progress_token kwarg.
    Verify the new signature works by mocking the subprocess at the module level."""
    import vmaf_mcp.server as s

    fake_script = Path("/tmp/_vmafmcp_test_bench/testdata/bench_all.sh")
    fake_script.parent.mkdir(parents=True, exist_ok=True)
    fake_script.write_text("#!/bin/bash\necho ok")

    orig_root = s._repo_root
    orig_vmaf = s._vmaf_binary

    s._repo_root = lambda: Path("/tmp/_vmafmcp_test_bench")
    s._vmaf_binary = lambda: Path("/usr/local/bin/vmaf")

    async def run():
        fake_proc = AsyncMock()
        fake_proc.returncode = 0
        fake_proc.communicate = AsyncMock(return_value=(b"bench ok\n", b""))
        with patch("asyncio.create_subprocess_exec", return_value=fake_proc):
            return await s._run_benchmark(progress_token="tok123")

    try:
        result = anyio.run(run)
        assert result["exit_code"] == 0
        assert "bench ok" in result["stdout"]
    finally:
        s._repo_root = orig_root
        s._vmaf_binary = orig_vmaf
        fake_script.unlink(missing_ok=True)
        fake_script.parent.rmdir()


# ---------------------------------------------------------------------------
# _send_progress — silent outside request context
# ---------------------------------------------------------------------------


def test_send_progress_noop_outside_context():
    """_send_progress must not raise when called outside a request context."""

    async def run():
        # Should complete silently — server.request_context raises LookupError
        # outside a real request, and we guard against that.
        await srv._send_progress("my-token", 0.5, 1.0, "halfway")

    anyio.run(run)


def test_send_progress_noop_without_token():
    """_send_progress with None token is a no-op and must not raise."""

    async def run():
        await srv._send_progress(None, 0.5, 1.0, "halfway")

    anyio.run(run)


# ---------------------------------------------------------------------------
# run_compare — subprocess mock
# ---------------------------------------------------------------------------


def test_run_compare_parses_json_output():
    """_run_compare must parse a JSON stdout from vmaf-tune compare."""
    fake_report = {"schema": "v2", "rows": [{"codec": "libx264", "ok": True}]}

    async def run():
        fake_proc = AsyncMock()
        fake_proc.returncode = 0
        fake_proc.communicate = AsyncMock(return_value=(json.dumps(fake_report).encode(), b""))
        with (
            patch("vmaf_mcp.server._vmaftune_binary") as mock_bin,
            patch("asyncio.create_subprocess_exec", return_value=fake_proc),
            patch("pathlib.Path.exists", return_value=True),
            patch("vmaf_mcp.server._validate_media_path", side_effect=str),
        ):
            mock_bin.return_value = Path("/usr/local/bin/vmaf-tune")
            result = await srv._run_compare(src="/tmp/test.yuv")
        return result

    result = anyio.run(run)
    assert result == fake_report


def test_run_compare_raises_on_nonzero_exit():
    """_run_compare must raise RuntimeError on non-zero exit."""

    async def run():
        fake_proc = AsyncMock()
        fake_proc.returncode = 1
        fake_proc.communicate = AsyncMock(return_value=(b"", b"something went wrong"))
        with (
            patch("vmaf_mcp.server._vmaftune_binary") as mock_bin,
            patch("asyncio.create_subprocess_exec", return_value=fake_proc),
            patch("pathlib.Path.exists", return_value=True),
            patch("vmaf_mcp.server._validate_media_path", side_effect=str),
        ):
            mock_bin.return_value = Path("/usr/local/bin/vmaf-tune")
            with pytest.raises(RuntimeError, match="vmaf-tune compare exited 1"):
                await srv._run_compare(src="/tmp/test.yuv")

    anyio.run(run)


def test_run_compare_missing_binary_raises():
    """_run_compare raises when the vmaf-tune binary doesn't exist."""

    async def run():
        with (
            patch("vmaf_mcp.server._vmaftune_binary") as mock_bin,
            patch("vmaf_mcp.server._validate_media_path", side_effect=str),
        ):
            fake_bin = Path("/nonexistent/vmaf-tune")
            mock_bin.return_value = fake_bin
            with pytest.raises(RuntimeError, match="vmaf-tune binary not found"):
                await srv._run_compare(src="/tmp/test.yuv")

    anyio.run(run)


# ---------------------------------------------------------------------------
# run_ladder — subprocess mock
# ---------------------------------------------------------------------------


def test_run_ladder_json_format():
    """_run_ladder with format='json' must parse stdout and wrap in {'manifest': ...}."""
    manifest_data = {"rungs": [{"width": 1920, "height": 1080}]}

    async def run():
        fake_proc = AsyncMock()
        fake_proc.returncode = 0
        fake_proc.communicate = AsyncMock(return_value=(json.dumps(manifest_data).encode(), b""))
        with (
            patch("vmaf_mcp.server._vmaftune_binary") as mock_bin,
            patch("asyncio.create_subprocess_exec", return_value=fake_proc),
            patch("pathlib.Path.exists", return_value=True),
            patch("vmaf_mcp.server._validate_media_path", side_effect=str),
        ):
            mock_bin.return_value = Path("/usr/local/bin/vmaf-tune")
            result = await srv._run_ladder(
                src="/tmp/test.yuv",
                resolutions="1920x1080,1280x720",
                target_vmafs="95,90",
                format="json",
            )
        return result

    result = anyio.run(run)
    assert result["format"] == "json"
    assert result["manifest"] == manifest_data


def test_run_ladder_hls_format():
    """_run_ladder with format='hls' returns the raw manifest string."""
    hls_text = "#EXTM3U\n#EXT-X-VERSION:3\n"

    async def run():
        fake_proc = AsyncMock()
        fake_proc.returncode = 0
        fake_proc.communicate = AsyncMock(return_value=(hls_text.encode(), b""))
        with (
            patch("vmaf_mcp.server._vmaftune_binary") as mock_bin,
            patch("asyncio.create_subprocess_exec", return_value=fake_proc),
            patch("pathlib.Path.exists", return_value=True),
            patch("vmaf_mcp.server._validate_media_path", side_effect=str),
        ):
            mock_bin.return_value = Path("/usr/local/bin/vmaf-tune")
            result = await srv._run_ladder(
                src="/tmp/test.yuv",
                resolutions="1920x1080",
                target_vmafs="95",
                format="hls",
            )
        return result

    result = anyio.run(run)
    assert result["format"] == "hls"
    assert result["manifest"] == hls_text


def test_run_ladder_raises_on_nonzero_exit():
    async def run():
        fake_proc = AsyncMock()
        fake_proc.returncode = 2
        fake_proc.communicate = AsyncMock(return_value=(b"", b"ladder failed"))
        with (
            patch("vmaf_mcp.server._vmaftune_binary") as mock_bin,
            patch("asyncio.create_subprocess_exec", return_value=fake_proc),
            patch("pathlib.Path.exists", return_value=True),
            patch("vmaf_mcp.server._validate_media_path", side_effect=str),
        ):
            mock_bin.return_value = Path("/usr/local/bin/vmaf-tune")
            with pytest.raises(RuntimeError, match="vmaf-tune ladder exited 2"):
                await srv._run_ladder(
                    src="/tmp/test.yuv",
                    resolutions="1920x1080",
                    target_vmafs="95",
                )

    anyio.run(run)


# ---------------------------------------------------------------------------
# run_tune_per_shot — subprocess mock
# ---------------------------------------------------------------------------


def test_run_tune_per_shot_parses_json():
    fake_plan = {"shots": [{"start": 0, "end": 60, "crf": 22}]}

    async def run():
        fake_proc = AsyncMock()
        fake_proc.returncode = 0
        fake_proc.communicate = AsyncMock(return_value=(json.dumps(fake_plan).encode(), b""))
        with (
            patch("vmaf_mcp.server._vmaftune_binary") as mock_bin,
            patch("asyncio.create_subprocess_exec", return_value=fake_proc),
            patch("pathlib.Path.exists", return_value=True),
            patch("vmaf_mcp.server._validate_media_path", side_effect=str),
        ):
            mock_bin.return_value = Path("/usr/local/bin/vmaf-tune")
            result = await srv._run_tune_per_shot(
                src="/tmp/test.yuv",
                target_vmaf=92.0,
            )
        return result

    result = anyio.run(run)
    assert result == fake_plan


def test_run_tune_per_shot_raises_on_nonzero():
    async def run():
        fake_proc = AsyncMock()
        fake_proc.returncode = 1
        fake_proc.communicate = AsyncMock(return_value=(b"", b"per-shot failed"))
        with (
            patch("vmaf_mcp.server._vmaftune_binary") as mock_bin,
            patch("asyncio.create_subprocess_exec", return_value=fake_proc),
            patch("pathlib.Path.exists", return_value=True),
            patch("vmaf_mcp.server._validate_media_path", side_effect=str),
        ):
            mock_bin.return_value = Path("/usr/local/bin/vmaf-tune")
            with pytest.raises(RuntimeError, match="vmaf-tune tune-per-shot exited 1"):
                await srv._run_tune_per_shot(src="/tmp/test.yuv")

    anyio.run(run)


def test_run_tune_per_shot_missing_binary_raises():
    async def run():
        with (
            patch("vmaf_mcp.server._vmaftune_binary") as mock_bin,
            patch("vmaf_mcp.server._validate_media_path", side_effect=str),
        ):
            mock_bin.return_value = Path("/nonexistent/vmaf-tune")
            with pytest.raises(RuntimeError, match="vmaf-tune binary not found"):
                await srv._run_tune_per_shot(src="/tmp/test.yuv")

    anyio.run(run)


# ---------------------------------------------------------------------------
# _vmaftune_binary — resolution order
# ---------------------------------------------------------------------------


def test_vmaftune_binary_env_override(monkeypatch):
    monkeypatch.setenv("VMAF_TUNE_BIN", "/custom/path/vmaf-tune")
    assert str(srv._vmaftune_binary()) == "/custom/path/vmaf-tune"


def test_vmaftune_binary_falls_back_to_which(monkeypatch):
    monkeypatch.delenv("VMAF_TUNE_BIN", raising=False)
    with patch("shutil.which", return_value="/usr/bin/vmaf-tune"):
        assert str(srv._vmaftune_binary()) == "/usr/bin/vmaf-tune"


def test_vmaftune_binary_falls_back_to_in_tree(monkeypatch):
    monkeypatch.delenv("VMAF_TUNE_BIN", raising=False)
    with patch("shutil.which", return_value=None):
        p = srv._vmaftune_binary()
        assert "tools/vmaf-tune/vmaf-tune" in str(p)


# ---------------------------------------------------------------------------
# tool registration — all six new tools appear in list_tools()
# ---------------------------------------------------------------------------


def test_new_p1_tools_registered():
    """Schema-level check that all six P1 tools are declared in list_tools()."""

    async def get_tools():
        return await srv._list_tools()

    tools = anyio.run(get_tools)
    names = {t.name for t in tools}
    for expected in (
        "list_extractors",
        "describe_model",
        "run_compare",
        "run_ladder",
        "run_tune_per_shot",
    ):
        assert expected in names, f"P1 tool {expected!r} not found in list_tools()"
    # Existing tools must still be present.
    for existing in (
        "vmaf_score",
        "list_models",
        "list_backends",
        "run_benchmark",
        "eval_model_on_split",
        "compare_models",
        "describe_worst_frames",
    ):
        assert existing in names, f"Existing tool {existing!r} was removed from list_tools()"


def test_list_extractors_schema():
    """list_extractors inputSchema must be a valid no-argument schema."""

    async def get_tools():
        return await srv._list_tools()

    tools = anyio.run(get_tools)
    by_name = {t.name: t for t in tools}
    schema = by_name["list_extractors"].input_schema
    assert schema.get("type") == "object"


def test_describe_model_schema_has_required_name():
    """describe_model inputSchema must declare 'name' as required."""

    async def get_tools():
        return await srv._list_tools()

    tools = anyio.run(get_tools)
    by_name = {t.name: t for t in tools}
    schema = by_name["describe_model"].input_schema
    assert "name" in schema.get("required", [])
    assert "name" in schema.get("properties", {})


def test_run_compare_schema_has_required_src():
    async def get_tools():
        return await srv._list_tools()

    tools = anyio.run(get_tools)
    by_name = {t.name: t for t in tools}
    schema = by_name["run_compare"].input_schema
    assert "src" in schema.get("required", [])


def test_run_ladder_schema_has_required_fields():
    async def get_tools():
        return await srv._list_tools()

    tools = anyio.run(get_tools)
    by_name = {t.name: t for t in tools}
    schema = by_name["run_ladder"].input_schema
    for req in ("src", "resolutions", "target_vmafs"):
        assert req in schema.get("required", []), f"{req!r} missing from run_ladder required"
