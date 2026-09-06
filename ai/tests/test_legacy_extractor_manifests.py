# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for replay manifests on legacy AI extractor utilities."""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "ai" / "scripts"


def _load_script(name: str):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS_DIR / f"{name}.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def test_collect_gpu_calibration_manifest(tmp_path: Path) -> None:
    mod = _load_script("collect_gpu_calibration_data")
    report = tmp_path / "gpu_calibration.manifest.json"
    args = argparse.Namespace(
        vmaf_binary=tmp_path / "vmaf",
        reference=tmp_path / "ref.yuv",
        distorted=tmp_path / "dis.yuv",
        width=16,
        height=16,
        pixel_format="420",
        bitdepth=8,
        arch_id="vulkan:lavapipe",
        cuda_device=0,
        sycl_device=0,
        vulkan_device=0,
        smoke=True,
        output=tmp_path / "calibration.parquet",
        manifest_out=report,
    )

    mod._write_manifest(
        path=report,
        args=args,
        raw_argv=["--smoke"],
        features=["vif"],
        backends=["vulkan"],
        frame_limit=100,
        row_count=4,
    )

    payload = json.loads(report.read_text(encoding="utf-8"))
    assert payload["schema"] == "gpu-calibration-data-manifest-v1"
    assert payload["selection"] == {"backends": ["vulkan"], "features": ["vif"], "frame_limit": 100}
    assert payload["row_count"] == 4
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"


def test_extract_ugc_manifest(tmp_path: Path) -> None:
    mod = _load_script("extract_ugc_features")
    report = tmp_path / "ugc.manifest.json"
    args = argparse.Namespace(
        manifest=tmp_path / "content.json",
        yuv_dir=tmp_path / "yuv",
        vmaf_bin=tmp_path / "vmaf",
        model=tmp_path / "model.json",
        out_parquet=tmp_path / "ugc.parquet",
        max_height=360,
        max_frames=300,
        threads=8,
        keep_yuv=False,
        manifest_out=report,
    )

    mod._write_manifest(
        path=report,
        args=args,
        raw_argv=["--manifest", str(args.manifest)],
        manifest_items=2,
        pair_count=3,
        fail_count=1,
        row_count=90,
        source_count=3,
    )

    payload = json.loads(report.read_text(encoding="utf-8"))
    assert payload["schema"] == "ugc-full-feature-extraction-manifest-v1"
    assert payload["pair_count"] == 3
    assert payload["fail_count"] == 1
    assert payload["teacher_model"] == mod.DEFAULT_MODEL
    assert "vmaf" in payload["feature_columns"]
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"


def test_extract_konvid_frames_manifest(tmp_path: Path) -> None:
    mod = _load_script("extract_konvid_frames")
    report = tmp_path / "konvid_frames.manifest.json"
    args = argparse.Namespace(root=tmp_path / "konvid", target_hw=224, manifest_out=report)

    mod._write_manifest(
        path=report,
        args=args,
        raw_argv=["--root", str(args.root)],
        root=args.root,
        manifest_entries=10,
        processed_count=8,
        missing_count=1,
        error_count=1,
        c2_rows=8,
        c3_rows=8,
    )

    payload = json.loads(report.read_text(encoding="utf-8"))
    assert payload["schema"] == "konvid-frame-extraction-manifest-v1"
    assert payload["processed_count"] == 8
    assert payload["missing_count"] == 1
    assert payload["target_hw"] == 224
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
