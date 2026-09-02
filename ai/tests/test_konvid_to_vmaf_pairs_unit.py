# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.konvid_to_vmaf_pairs`.

The script chains ffprobe/ffmpeg/libvmaf subprocesses; every external
call is mocked so the tests run without any media-tool dependency.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
from pathlib import Path
from unittest.mock import patch

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "konvid_to_vmaf_pairs.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("kvp_under_test", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


KVP = _load_module()


# ---------------------------------------------------------------------------
# DEFAULT_FEATURES sanity
# ---------------------------------------------------------------------------


def test_default_features_includes_canonical_six() -> None:
    expected = {"vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "adm2", "motion2"}
    assert set(KVP.DEFAULT_FEATURES) == expected


# ---------------------------------------------------------------------------
# _decode_yuv (ffprobe + ffmpeg path) — fully mocked
# ---------------------------------------------------------------------------


def test_decode_yuv_invokes_ffprobe_then_ffmpeg(tmp_path: Path) -> None:
    src = tmp_path / "clip.mp4"
    src.write_bytes(b"\x00")
    out_yuv = tmp_path / "ref.yuv"

    calls: list[list[str]] = []

    def fake_run(cmd, check=True, **kw):
        calls.append(list(cmd))
        if cmd[0] == "ffprobe":
            return subprocess.CompletedProcess(cmd, 0, stdout="320\n240\n5\n", stderr="")
        return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

    with patch.object(KVP.subprocess, "run", side_effect=fake_run):
        w, h, n = KVP._decode_yuv(src, out_yuv)
    assert (w, h, n) == (320, 240, 5)
    assert calls[0][0] == "ffprobe"
    assert calls[1][0] == "ffmpeg"


# ---------------------------------------------------------------------------
# _encode_dis
# ---------------------------------------------------------------------------


def test_encode_dis_writes_intermediate_then_decodes(tmp_path: Path) -> None:
    src = tmp_path / "src.mp4"
    src.write_bytes(b"\x00")
    out_yuv = tmp_path / "dis.yuv"

    calls: list[list[str]] = []

    def fake_run(cmd, check=True, **kw):
        calls.append(list(cmd))
        # The first ffmpeg call writes the .dis.mp4 intermediate.
        if "-c:v" in cmd:
            inter = out_yuv.with_suffix(".dis.mp4")
            inter.write_bytes(b"\x00")
        return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

    with patch.object(KVP.subprocess, "run", side_effect=fake_run):
        KVP._encode_dis(src, out_yuv, crf=28)
    assert len(calls) == 2  # encode then decode
    assert calls[0][0] == "ffmpeg"
    assert "-crf" in calls[0]
    assert calls[0][calls[0].index("-crf") + 1] == "28"


# ---------------------------------------------------------------------------
# _run_vmaf
# ---------------------------------------------------------------------------


def test_run_vmaf_argv(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")
    ref = tmp_path / "r.yuv"
    dis = tmp_path / "d.yuv"
    out_json = tmp_path / "vmaf.json"
    model = tmp_path / "model.json"
    ref.write_bytes(b"\x00")
    dis.write_bytes(b"\x00")
    model.write_text("{}")

    captured: list[list[str]] = []

    def fake_run(cmd, check=True, **kw):
        captured.append(list(cmd))
        return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

    with patch.object(KVP.subprocess, "run", side_effect=fake_run):
        KVP._run_vmaf(binary, ref, dis, 320, 240, out_json, model)

    cmd = captured[0]
    assert cmd[0] == str(binary)
    assert "--reference" in cmd and cmd[cmd.index("--reference") + 1] == str(ref)
    assert "--distorted" in cmd and cmd[cmd.index("--distorted") + 1] == str(dis)
    assert "--width" in cmd and cmd[cmd.index("--width") + 1] == "320"
    assert "--height" in cmd and cmd[cmd.index("--height") + 1] == "240"
    assert "--model" in cmd and cmd[cmd.index("--model") + 1] == f"path={model}"
    # Vulkan backend was removed (ADR-0726); only CUDA and SYCL bypass flags remain.
    assert "--no_cuda" in cmd and "--no_sycl" in cmd


# ---------------------------------------------------------------------------
# _frames_to_rows
# ---------------------------------------------------------------------------


def _vmaf_json(frames: int = 3) -> dict:
    out = {"frames": []}
    for i in range(frames):
        m = {f"integer_{f}": 10.0 + i for f in KVP.DEFAULT_FEATURES}
        m["vmaf"] = 80.0 + i
        out["frames"].append({"frameNum": i, "metrics": m})
    return out


def test_frames_to_rows_emits_one_row_per_frame(tmp_path: Path) -> None:
    p = tmp_path / "v.json"
    p.write_text(json.dumps(_vmaf_json(4)))
    rows = KVP._frames_to_rows("clipKey", p)
    assert len(rows) == 4
    for i, row in enumerate(rows):
        assert row["key"] == "clipKey"
        assert row["frame_index"] == i
        assert row["vmaf"] == pytest.approx(80.0 + i)
        for feat in KVP.DEFAULT_FEATURES:
            assert row[feat] == pytest.approx(10.0 + i)


def test_frames_to_rows_keys_are_canonical_set(tmp_path: Path) -> None:
    p = tmp_path / "v.json"
    p.write_text(json.dumps(_vmaf_json(1)))
    rows = KVP._frames_to_rows("k", p)
    expected_keys = {"key", "frame_index", "vmaf", *KVP.DEFAULT_FEATURES}
    assert set(rows[0].keys()) == expected_keys


# ---------------------------------------------------------------------------
# _process_clip — cache hit / cache write
# ---------------------------------------------------------------------------


def test_process_clip_returns_cached_rows(tmp_path: Path) -> None:
    cache = tmp_path / "cache"
    cache.mkdir()
    cached = [{"key": "k", "frame_index": 0, "vmaf": 75.0}]
    (cache / "k.json").write_text(json.dumps(cached))

    rows = KVP._process_clip(
        "k",
        tmp_path / "src.mp4",
        tmp_path / "vmaf",
        tmp_path / "model.json",
        crf=35,
        cache_dir=cache,
        scratch=tmp_path / "scratch",
    )
    assert rows == cached


def test_process_clip_runs_pipeline_and_writes_cache(tmp_path: Path) -> None:
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    cache = tmp_path / "cache"

    fake_rows = [{"key": "k", "frame_index": 0, "vmaf": 90.0}]
    with (
        patch.object(KVP, "_decode_yuv", return_value=(320, 240, 1)) as dec,
        patch.object(KVP, "_encode_dis") as enc,
        patch.object(KVP, "_run_vmaf") as runv,
        patch.object(KVP, "_frames_to_rows", return_value=fake_rows) as f2r,
    ):
        rows = KVP._process_clip(
            "k",
            tmp_path / "src.mp4",
            tmp_path / "vmaf",
            tmp_path / "model.json",
            crf=35,
            cache_dir=cache,
            scratch=scratch,
        )
    assert rows == fake_rows
    dec.assert_called_once()
    enc.assert_called_once()
    runv.assert_called_once()
    f2r.assert_called_once()
    # cache file written
    assert (cache / "k.json").is_file()
    assert json.loads((cache / "k.json").read_text()) == fake_rows


def test_process_clip_without_cache(tmp_path: Path) -> None:
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    with (
        patch.object(KVP, "_decode_yuv", return_value=(320, 240, 1)),
        patch.object(KVP, "_encode_dis"),
        patch.object(KVP, "_run_vmaf"),
        patch.object(KVP, "_frames_to_rows", return_value=[{"key": "k", "frame_index": 0}]),
    ):
        rows = KVP._process_clip(
            "k",
            tmp_path / "src.mp4",
            tmp_path / "vmaf",
            tmp_path / "model.json",
            crf=35,
            cache_dir=None,
            scratch=scratch,
        )
    assert rows == [{"key": "k", "frame_index": 0}]


# ---------------------------------------------------------------------------
# main — error paths
# ---------------------------------------------------------------------------


def test_main_returns_2_when_videos_dir_missing(tmp_path: Path) -> None:
    rc = KVP.main(
        [
            "--konvid-root",
            str(tmp_path / "absent"),
            "--vmaf-bin",
            str(tmp_path / "vmaf"),
            "--out",
            str(tmp_path / "out.parquet"),
        ]
    )
    assert rc == 2


def test_main_returns_2_when_vmaf_bin_missing(tmp_path: Path) -> None:
    root = tmp_path / "konvid"
    (root / "KoNViD_1k_videos").mkdir(parents=True)
    rc = KVP.main(
        [
            "--konvid-root",
            str(root),
            "--vmaf-bin",
            str(tmp_path / "no_vmaf_here"),
            "--out",
            str(tmp_path / "out.parquet"),
        ]
    )
    assert rc == 2
