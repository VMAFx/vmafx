# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for ``ai/scripts/enrich_k150k_parquet_metadata.py``."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

pd = pytest.importorskip("pandas")

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_DIR = _REPO_ROOT / "ai" / "scripts"
_SCRIPT_PATH = _SCRIPT_DIR / "enrich_k150k_parquet_metadata.py"


def _load_module():
    sys.path.insert(0, str(_SCRIPT_DIR))
    spec = importlib.util.spec_from_file_location("enrich_k150k_parquet_metadata", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


ENRICH = _load_module()


def test_enrich_frame_fills_missing_metadata() -> None:
    frame = pd.DataFrame(
        [
            {"clip_name": "clip-a.mp4", "mos": 3.0, "split": pd.NA},
            {"clip_name": "clip-b.mp4", "mos": 2.0, "split": "test"},
            {"clip_name": "clip-c.mp4", "mos": 4.0},
        ]
    )
    metadata = {
        "clip-a.mp4": {
            "split": "train",
            "chug_content_name": "source-a.mp4",
            "mos_raw_0_100": 50.0,
        },
        "clip-b.mp4": {
            "split": "val",
            "chug_content_name": "source-b.mp4",
        },
    }

    out, stats = ENRICH.enrich_frame(frame, metadata)

    assert out.loc[0, "split"] == "train"
    assert out.loc[1, "split"] == "test"
    assert out.loc[0, "chug_content_name"] == "source-a.mp4"
    assert out.loc[1, "chug_content_name"] == "source-b.mp4"
    assert out.loc[0, "mos_raw_0_100"] == pytest.approx(50.0)
    assert stats == {
        "rows": 3,
        "metadata_rows": 2,
        "matched_rows": 2,
        "missing_rows": 1,
        "updated_cells": 4,
    }


def test_enrich_frame_overwrite_replaces_existing_metadata() -> None:
    frame = pd.DataFrame([{"clip_name": "clip-b.mp4", "mos": 2.0, "split": "test"}])
    metadata = {"clip-b.mp4": {"split": "val"}}

    out, stats = ENRICH.enrich_frame(frame, metadata, overwrite=True)

    assert out.loc[0, "split"] == "val"
    assert stats["updated_cells"] == 1


def test_cli_enriches_existing_parquet(tmp_path: Path) -> None:
    features = tmp_path / "features.parquet"
    metadata = tmp_path / "chug.jsonl"
    out = tmp_path / "enriched.parquet"
    pd.DataFrame(
        [
            {"clip_name": "clip-a.mp4", "mos": 3.0, "adm2_mean": 1.0},
            {"clip_name": "clip-b.mp4", "mos": 2.0, "adm2_mean": 0.9},
        ]
    ).to_parquet(features)
    metadata.write_text(
        "\n".join(
            [
                json.dumps(
                    {
                        "src": "clip-a.mp4",
                        "chug_content_name": "source-a.mp4",
                        "mos_raw_0_100": 75.0,
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

    proc = subprocess.run(
        [
            sys.executable,
            str(_SCRIPT_PATH),
            "--features-parquet",
            str(features),
            "--metadata-jsonl",
            str(metadata),
            "--out",
            str(out),
            "--split-seed",
            "stable",
        ],
        capture_output=True,
        text=True,
        check=True,
    )

    summary = json.loads(proc.stdout)
    enriched = pd.read_parquet(out)
    manifest = json.loads(out.with_suffix(".manifest.json").read_text(encoding="utf-8"))
    assert summary["matched_rows"] == 2
    assert summary["manifest"] == str(out.with_suffix(".manifest.json"))
    assert enriched.loc[0, "chug_content_name"] == "source-a.mp4"
    assert enriched.loc[1, "split"] == "test"
    assert features.is_file()
    assert manifest["schema"] == "k150k-metadata-enrichment-manifest-v1"
    assert manifest["stats"]["matched_rows"] == 2
    assert manifest["metadata_keys"] == [
        "chug_content_name",
        "chug_split_key",
        "chug_split_policy",
        "mos_raw_0_100",
        "split",
    ]
    assert manifest["run_provenance"]["schema"] == "ai-run-provenance-v1"
