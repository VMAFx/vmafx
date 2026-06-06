# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for ``ai/scripts/extract_k150k_features.py``."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "extract_k150k_features.py"


def _load_module():
    # Return the cached module if it is already registered under this name.
    # Without this guard, re-executing exec_module() at collection time replaces
    # sys.modules["extract_k150k_features"] with a new module object.  Any test
    # in a sibling file (e.g. test_extract_k150k_perf.py) that imported
    # _process_clip earlier holds a reference into the *old* object's globals,
    # so unittest.mock patches applied to sys.modules["extract_k150k_features"]
    # (the new object) are invisible to those already-bound function references
    # — causing real ffmpeg/ffprobe calls despite the mock.
    if "extract_k150k_features" in sys.modules:
        return sys.modules["extract_k150k_features"]
    spec = importlib.util.spec_from_file_location("extract_k150k_features", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


K150K = _load_module()


def test_cuda_feature_passes_split_gpu_and_cpu_residual(monkeypatch, tmp_path: Path) -> None:
    """CUDA pass: vmaf_v0.6.1 model dispatched on CUDA leg; vmaf key non-NaN."""
    calls: list[list[str]] = []

    def fake_run(cmd, **_kwargs):
        calls.append([str(part) for part in cmd])
        out = Path(cmd[cmd.index("--output") + 1])
        names = [cmd[idx + 1] for idx, part in enumerate(cmd) if part == "--feature"]
        model_args = [cmd[idx + 1] for idx, part in enumerate(cmd) if part == "--model"]
        metrics = {}
        if "adm_cuda" in names:
            # CUDA leg: must carry --model version=vmaf_v0.6.1 (Research-0135).
            assert (
                "version=vmaf_v0.6.1" in model_args
            ), "--model vmaf_v0.6.1 must be present on CUDA leg"
            metrics["integer_adm2"] = 1.0
            metrics["integer_vif_scale0"] = 2.0
            metrics["integer_motion2"] = 3.0
            metrics["psnr_y"] = 72.0
            metrics["ciede2000"] = 0.0
            metrics["float_ms_ssim"] = 1.0
            metrics["psnr_hvs"] = 99.0
            metrics["ssimulacra2"] = 100.0
            # Model score emitted by vmaf_v0.6.1 dispatch.
            metrics["vmaf"] = 87.5
            assert "--backend" in cmd
            assert cmd[cmd.index("--backend") + 1] == "cuda"
            assert "float_ssim_cuda" not in names
            assert "cambi_cuda" not in names
        else:
            # CPU residual leg (float_ssim + cambi only): no model arg needed.
            metrics["float_ssim"] = 0.9
            metrics["cambi"] = 0.1
            assert "--no_cuda" in cmd
        out.write_text(json.dumps({"frames": [{"metrics": metrics}]}), encoding="utf-8")
        return subprocess.CompletedProcess(args=cmd, returncode=0)

    monkeypatch.setattr(K150K.subprocess, "run", fake_run)

    frames = K150K._run_feature_passes(
        vmaf_bin=Path("core/build-cuda/tools/vmaf"),
        cpu_vmaf_bin=Path("core/build-cpu/tools/vmaf"),
        yuv_path=tmp_path / "clip.yuv",
        width=1280,
        height=720,
        pix_fmt="yuv420p10le",
        out_json=tmp_path / "clip.json",
        threads=2,
        use_cuda=True,
    )

    assert len(calls) == 2
    # vmaf key must be present and non-NaN (Research-0135 Option B).
    merged = frames[0]
    assert "vmaf" in merged, "vmaf key must be present after model dispatch"
    assert merged["vmaf"] == 87.5
    assert merged["float_ssim"] == 0.9
    assert merged["cambi"] == 0.1


def test_cpu_feature_pass_uses_generic_extractors(monkeypatch, tmp_path: Path) -> None:
    """CPU pass: vmaf_v0.6.1 model dispatched; vmaf key non-NaN in output."""
    calls: list[list[str]] = []

    def fake_run(cmd, **_kwargs):
        calls.append([str(part) for part in cmd])
        out = Path(cmd[cmd.index("--output") + 1])
        names = [cmd[idx + 1] for idx, part in enumerate(cmd) if part == "--feature"]
        assert names == list(K150K.EXTRACTOR_NAMES)
        assert "--no_cuda" in cmd
        # CPU path must also carry --model version=vmaf_v0.6.1 (Research-0135).
        model_args = [cmd[idx + 1] for idx, part in enumerate(cmd) if part == "--model"]
        assert "version=vmaf_v0.6.1" in model_args, "--model vmaf_v0.6.1 must be present"
        out.write_text(json.dumps({"frames": [{"metrics": {"vmaf": 83.2}}]}), encoding="utf-8")
        return subprocess.CompletedProcess(args=cmd, returncode=0)

    monkeypatch.setattr(K150K.subprocess, "run", fake_run)

    frames = K150K._run_feature_passes(
        vmaf_bin=Path("core/build-cpu/tools/vmaf"),
        cpu_vmaf_bin=Path("core/build-cpu/tools/vmaf"),
        yuv_path=tmp_path / "clip.yuv",
        width=640,
        height=360,
        pix_fmt="yuv420p",
        out_json=tmp_path / "clip.json",
        threads=1,
        use_cuda=False,
    )

    assert len(calls) == 1
    # vmaf key must be present and non-NaN (Option B, Research-0135).
    assert frames == [{"vmaf": 83.2}]


def test_vmaf_column_non_nan_in_aggregated_output(monkeypatch, tmp_path: Path) -> None:
    """End-to-end: vmaf_mean and vmaf_std are non-NaN when model is dispatched."""
    import numpy as np

    def fake_run(cmd, **_kwargs):
        out = Path(cmd[cmd.index("--output") + 1])
        out.write_text(
            json.dumps(
                {
                    "frames": [
                        {"metrics": {"vmaf": 85.0}},
                        {"metrics": {"vmaf": 88.0}},
                        {"metrics": {"vmaf": 82.0}},
                    ]
                }
            ),
            encoding="utf-8",
        )
        return subprocess.CompletedProcess(args=cmd, returncode=0)

    monkeypatch.setattr(K150K.subprocess, "run", fake_run)

    frames = K150K._run_vmaf_json(
        vmaf_bin=Path("core/build-cpu/tools/vmaf"),
        yuv_path=tmp_path / "clip.yuv",
        width=640,
        height=360,
        pix_fmt="yuv420p",
        out_json=tmp_path / "clip.json",
        threads=1,
        extractor_names=K150K.EXTRACTOR_NAMES,
        backend_args=["--no_cuda", "--no_sycl", "--no_vulkan", "--model", "version=vmaf_v0.6.1"],
    )
    agg = K150K._aggregate_frames(frames)
    assert "vmaf_mean" in agg, "vmaf_mean must be present after Option B dispatch"
    assert not np.isnan(agg["vmaf_mean"]), "vmaf_mean must be non-NaN (Research-0135 Option B)"
    assert not np.isnan(agg["vmaf_std"]), "vmaf_std must be non-NaN (Research-0135 Option B)"


def test_jsonl_metadata_preserves_chug_content_split(tmp_path: Path) -> None:
    meta_jsonl = tmp_path / "chug.jsonl"
    meta_jsonl.write_text(
        "\n".join(
            [
                json.dumps(
                    {
                        "src": "clip-a.mp4",
                        "mos_raw_0_100": 72.5,
                        "chug_video_id": "clip-a",
                        "chug_content_name": "source-a.mp4",
                        "chug_bitladder": "720p_1M_",
                    }
                ),
                json.dumps(
                    {
                        "src": "clip-b.mp4",
                        "chug_content_name": "source-b.mp4",
                        "split": "test",
                    }
                ),
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    metadata = K150K._load_jsonl_metadata(meta_jsonl, split_seed="stable")

    assert metadata["clip-a.mp4"]["mos_raw_0_100"] == 72.5
    assert metadata["clip-a.mp4"]["chug_video_id"] == "clip-a"
    assert metadata["clip-a.mp4"]["chug_split_key"] == "source-a.mp4"
    assert metadata["clip-a.mp4"]["split"] in {"train", "val", "test"}
    assert metadata["clip-b.mp4"]["split"] == "test"


def test_detect_fr_corpus_misuse_flags_chug_pairs() -> None:
    """CHUG sidecar with ref + dis rows for the same content triggers the guard.

    ADR-0510: the script is an FR-from-NR adapter; running it on an FR
    corpus silently produces a parquet where every clip scores against
    itself (vmaf~99 across all bitrate-ladder rungs).
    """
    meta_by_clip = {
        "ref-clip.mp4": {
            "chug_ref": 1,
            "chug_content_name": "source-X.mp4",
            "chug_bitladder": "1080p_ref_",
        },
        "dis-clip-0.2M.mp4": {
            "chug_ref": 0,
            "chug_content_name": "source-X.mp4",
            "chug_bitladder": "360p_0.2M_",
        },
        "dis-clip-1M.mp4": {
            "chug_ref": 0,
            "chug_content_name": "source-X.mp4",
            "chug_bitladder": "1080p_1M_",
        },
    }

    result = K150K.detect_fr_corpus_misuse(meta_by_clip)

    assert result["misuse_detected"] is True
    assert result["ref_count"] == 1
    assert result["dis_count"] == 2
    assert result["content_groups_with_both"] == 1
    assert result["example"] == "source-X.mp4"


def test_detect_fr_corpus_misuse_allows_nr_only_sidecar() -> None:
    """K150K-A sidecars (no chug_ref field) must NOT trigger the guard."""
    meta_by_clip = {
        "k150ka-clip-1.mp4": {"mos_raw_0_100": 75.3},
        "k150ka-clip-2.mp4": {"mos_raw_0_100": 42.1},
    }

    result = K150K.detect_fr_corpus_misuse(meta_by_clip)

    assert result["misuse_detected"] is False
    assert result["ref_count"] == 0
    assert result["dis_count"] == 0
    assert result["content_groups_with_both"] == 0
    assert result["example"] is None


def test_detect_fr_corpus_misuse_allows_ref_only_or_dis_only_groups() -> None:
    """Groups with only ref OR only dis rows do NOT trigger — needs both."""
    meta_by_clip = {
        "ref-only.mp4": {
            "chug_ref": 1,
            "chug_content_name": "lone-ref.mp4",
        },
        "dis-only.mp4": {
            "chug_ref": 0,
            "chug_content_name": "lone-dis.mp4",
        },
    }

    result = K150K.detect_fr_corpus_misuse(meta_by_clip)

    assert result["misuse_detected"] is False
    assert result["ref_count"] == 1
    assert result["dis_count"] == 1
    assert result["content_groups_with_both"] == 0


def test_write_extraction_manifest_records_run_provenance(tmp_path: Path) -> None:
    out = tmp_path / "full_features_k150k.parquet"
    manifest = tmp_path / "full_features_k150k.manifest.json"
    clips_dir = tmp_path / "clips"
    scores_csv = tmp_path / "scores.csv"
    metadata_jsonl = tmp_path / "metadata.jsonl"
    vmaf_bin = tmp_path / "vmaf"
    clips_dir.mkdir()
    scores_csv.write_text("video_name,video_score\nclip-a.mp4,78\n", encoding="utf-8")
    metadata_jsonl.write_text(json.dumps({"src": "clip-a.mp4"}) + "\n", encoding="utf-8")
    vmaf_bin.write_text("#!/bin/sh\n", encoding="utf-8")
    K150K._write_parquet_from_rows([{"clip_name": "clip-a.mp4", "mos": 78.0}], out)

    args = K150K.argparse.Namespace(
        out=out,
        clips_dir=clips_dir,
        scores=scores_csv,
        metadata_jsonl=metadata_jsonl,
        vmaf_bin=vmaf_bin,
        cpu_vmaf_bin=vmaf_bin,
        threads_cuda=2,
        threads=1,
        allow_fr_from_nr=False,
        split_seed="stable",
        manifest_out=manifest,
    )

    K150K._write_extraction_manifest(
        manifest_out=manifest,
        args=args,
        use_cuda=True,
        total_clips=1,
        done_before=0,
        pending_count=1,
        recovered_rows=0,
        ok=1,
        fail=0,
        elapsed_seconds=2.0,
        status="complete",
    )

    payload = json.loads(manifest.read_text(encoding="utf-8"))
    assert payload["schema"] == "k150k-feature-extraction-manifest-v1"
    assert payload["stats"]["parquet_rows"] == 1
    assert payload["stats"]["rate_clip_per_second"] == 0.5
    assert payload["backend"] == {"use_cuda": True, "workers": 2, "threads_per_worker": 1}
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert payload["run_provenance"]["outputs"]["parquet"]["exists"] is True
