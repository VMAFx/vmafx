# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Run-provenance manifest tests for dataset fetch helpers."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPTS = _REPO_ROOT / "ai" / "scripts"


def _load_module(name: str):
    path = _SCRIPTS / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"{name}_manifest_test", path)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_fetch_youtube_ugc_subset_writes_run_manifest(tmp_path: Path, monkeypatch) -> None:
    mod = _load_module("fetch_youtube_ugc_subset")

    bucket_items = [
        {"name": "vp9_compressed_videos/ClipA_orig.mp4", "size": "10"},
        {"name": "vp9_compressed_videos/ClipA_cbr.webm", "size": "11"},
        {"name": "vp9_compressed_videos/ClipA_vod.webm", "size": "12"},
        {"name": "vp9_compressed_videos/ClipA_vodlb.webm", "size": "13"},
    ]
    monkeypatch.setattr(mod, "_list_bucket", lambda _prefix: bucket_items)

    def fake_download(_url: str, dest: Path) -> None:
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(b"video")

    monkeypatch.setattr(mod, "_download", fake_download)
    out_dir = tmp_path / "clips"
    content_manifest = tmp_path / "subset.json"
    monkeypatch.setattr(
        "sys.argv",
        [
            "fetch_youtube_ugc_subset.py",
            "--out-dir",
            str(out_dir),
            "--n-stems",
            "1",
            "--manifest",
            str(content_manifest),
        ],
    )

    assert mod.main() == 0

    manifest = json.loads(content_manifest.read_text(encoding="utf-8"))
    assert sorted(manifest["ClipA"]) == ["cbr", "orig", "vod", "vodlb"]

    run_manifest = json.loads((tmp_path / "subset.run-manifest.json").read_text())
    assert run_manifest["schema"] == "youtube-ugc-subset-fetch-run-manifest-v1"
    assert run_manifest["selection"] == {
        "files_selected": 4,
        "n_stems_requested": 1,
        "policy": "smallest-complete-4tuple",
        "stems_selected": 1,
        "total_size_bytes": 46,
    }
    assert run_manifest["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert run_manifest["run_provenance"]["outputs"]["content_manifest"]["path"].endswith(
        "subset.json"
    )


def test_fetch_konvid_1k_writes_fetch_manifest(tmp_path: Path, monkeypatch) -> None:
    mod = _load_module("fetch_konvid_1k")

    def fake_download(_url: str, dst: Path, _min_bytes: int) -> Path:
        dst.write_bytes(b"zip")
        return dst

    def fake_extract(zip_path: Path, dst_dir: Path) -> None:
        if zip_path.name.endswith("_videos.zip"):
            (dst_dir / "KoNViD_1k_videos").mkdir(parents=True, exist_ok=True)
        else:
            (dst_dir / "KoNViD_1k_metadata").mkdir(parents=True, exist_ok=True)

    monkeypatch.setattr(mod, "_download", fake_download)
    monkeypatch.setattr(mod, "_extract", fake_extract)
    monkeypatch.setattr(
        "sys.argv",
        [
            "fetch_konvid_1k.py",
            "--root",
            str(tmp_path),
            "--keep-zips",
        ],
    )

    assert mod.main() == 0

    manifest = json.loads((tmp_path / "fetch_manifest.json").read_text(encoding="utf-8"))
    assert manifest["schema"] == "konvid-1k-fetch-manifest-v1"
    assert manifest["dataset"] == "konvid-1k"
    assert manifest["keep_zips"] is True
    assert {archive["label"] for archive in manifest["archives"]} == {"videos", "metadata"}
    assert manifest["extracted"]["videos_dir_exists"] is True
    assert manifest["extracted"]["metadata_dir_exists"] is True
    assert manifest["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert manifest["run_provenance"]["outputs"]["manifest"]["path"].endswith("fetch_manifest.json")
