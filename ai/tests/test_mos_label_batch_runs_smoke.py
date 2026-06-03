# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Scaffold + smoke tests for the KonViD and CHUG MOS-label batch-run manifests.

ADR-0992.  ``ai/configs/mos-label-batch-konvid.json`` and
``ai/configs/mos-label-batch-chug.json`` are the operator-facing
batch manifests that join MOS labels onto extracted feature tables.

These tests verify:

1. Both JSON manifests are syntactically valid and parse without error
   via ``batch_materialize_mos_labels.load_batch_manifest()`` when
   passed a ``base_dir`` that resolves to a tmp directory (so absent
   corpus files do not cause path-exists failures during the schema
   phase, only during the I/O phase).
2. The KonViD manifest encodes the expected table ids, key columns,
   and key-normalisation defaults.
3. The CHUG manifest encodes ``key_normalize: raw`` (CHUG video IDs
   are opaque strings, not file paths) and ``chug_video_id`` key
   columns.
4. A synthetic end-to-end batch run that replaces corpus paths with
   in-memory JSONL fixtures succeeds and emits the ``mos`` column.

No real corpus data is needed.  The tests are CI-safe and dependency-free
(pandas + json only).
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPTS_DIR = _REPO_ROOT / "ai" / "scripts"
_SCRIPT_PATH = _SCRIPTS_DIR / "batch_materialize_mos_labels.py"
_CONFIGS_DIR = _REPO_ROOT / "ai" / "configs"
_KONVID_MANIFEST = _CONFIGS_DIR / "mos-label-batch-konvid.json"
_CHUG_MANIFEST = _CONFIGS_DIR / "mos-label-batch-chug.json"


def _load_module():
    # ``_script_bootstrap`` lives in ai/scripts/; make it importable before
    # executing the script module so the bootstrap import does not fail when
    # pytest is invoked from the repo root without PYTHONPATH=ai/scripts.
    if str(_SCRIPTS_DIR) not in sys.path:
        sys.path.insert(0, str(_SCRIPTS_DIR))
    spec = importlib.util.spec_from_file_location("batch_materialize_mos_labels", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _write_jsonl(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "\n".join(json.dumps(row, sort_keys=True) for row in rows) + "\n",
        encoding="utf-8",
    )


# ---------------------------------------------------------------------------
# Manifest presence
# ---------------------------------------------------------------------------


def test_konvid_manifest_file_exists() -> None:
    assert _KONVID_MANIFEST.is_file(), f"missing {_KONVID_MANIFEST}"


def test_chug_manifest_file_exists() -> None:
    assert _CHUG_MANIFEST.is_file(), f"missing {_CHUG_MANIFEST}"


# ---------------------------------------------------------------------------
# Manifest JSON validity
# ---------------------------------------------------------------------------


def test_konvid_manifest_is_valid_json() -> None:
    payload = json.loads(_KONVID_MANIFEST.read_text(encoding="utf-8"))
    assert isinstance(payload, dict)
    assert "tables" in payload
    assert isinstance(payload["tables"], list)
    assert len(payload["tables"]) >= 1


def test_chug_manifest_is_valid_json() -> None:
    payload = json.loads(_CHUG_MANIFEST.read_text(encoding="utf-8"))
    assert isinstance(payload, dict)
    assert "tables" in payload
    assert isinstance(payload["tables"], list)
    assert len(payload["tables"]) >= 1


# ---------------------------------------------------------------------------
# Schema validation via load_batch_manifest() with a synthetic base_dir
# ---------------------------------------------------------------------------


def test_konvid_manifest_parses_without_error(tmp_path: Path) -> None:
    """load_batch_manifest() validates JSON schema; path existence is not checked here."""
    module = _load_module()
    # Populate base_dir so relative path resolution does not raise FileNotFoundError
    # during the schema-parse phase (paths are resolved but not opened yet).
    specs = module.load_batch_manifest(_KONVID_MANIFEST, base_dir=tmp_path)
    assert len(specs) >= 2
    ids = [s.table_id for s in specs]
    assert "konvid-1k" in ids
    assert "konvid-150k" in ids


def test_konvid_manifest_key_columns(tmp_path: Path) -> None:
    module = _load_module()
    specs = module.load_batch_manifest(_KONVID_MANIFEST, base_dir=tmp_path)
    for spec in specs:
        assert (
            spec.config.get("feature_key_column") == "key"
        ), f"konvid table {spec.table_id!r}: expected feature_key_column='key'"
        assert (
            spec.config.get("label_key_column") == "src"
        ), f"konvid table {spec.table_id!r}: expected label_key_column='src'"


def test_konvid_manifest_key_regex_set(tmp_path: Path) -> None:
    module = _load_module()
    # Trigger schema validation; path existence is not checked during parsing.
    module.load_batch_manifest(_KONVID_MANIFEST, base_dir=tmp_path)
    defaults = json.loads(_KONVID_MANIFEST.read_text(encoding="utf-8")).get("defaults", {})
    # Regex must be present either in defaults or per-table
    feature_regex = defaults.get("feature_key_regex")
    label_regex = defaults.get("label_key_regex")
    assert feature_regex is not None, "konvid manifest missing feature_key_regex default"
    assert label_regex is not None, "konvid manifest missing label_key_regex default"
    # Must capture a numeric sequence (KonViD clip ids are 6+ digit integers)
    assert "[0-9]" in feature_regex
    assert "[0-9]" in label_regex


def test_chug_manifest_parses_without_error(tmp_path: Path) -> None:
    module = _load_module()
    specs = module.load_batch_manifest(_CHUG_MANIFEST, base_dir=tmp_path)
    assert len(specs) >= 1
    ids = [s.table_id for s in specs]
    assert "chug-hdr" in ids


def test_chug_manifest_raw_key_normalize(tmp_path: Path) -> None:
    """CHUG video IDs are opaque strings, not file paths; key_normalize must be raw."""
    module = _load_module()
    specs = module.load_batch_manifest(_CHUG_MANIFEST, base_dir=tmp_path)
    chug_spec = next(s for s in specs if s.table_id == "chug-hdr")
    assert (
        chug_spec.config.get("key_normalize") == "raw"
    ), "chug-hdr table must use key_normalize=raw (IDs are not file paths)"


def test_chug_manifest_key_columns(tmp_path: Path) -> None:
    module = _load_module()
    specs = module.load_batch_manifest(_CHUG_MANIFEST, base_dir=tmp_path)
    chug_spec = next(s for s in specs if s.table_id == "chug-hdr")
    assert chug_spec.config.get("feature_key_column") == "chug_video_id"
    assert chug_spec.config.get("label_key_column") == "chug_video_id"


# ---------------------------------------------------------------------------
# Synthetic end-to-end run — KonViD shape
# ---------------------------------------------------------------------------


def test_konvid_batch_synthetic_run(tmp_path: Path) -> None:
    """End-to-end batch run using synthetic KonViD-shaped data.

    Replaces corpus paths with in-memory JSONL fixtures so CI can run
    without real corpus data on disk.  Verifies the batch runner emits
    ``mos`` on matched rows and writes a valid ``mos-label-materializer-batch-v1``
    report.
    """
    module = _load_module()

    # Build synthetic feature + label tables that match the KonViD key schema.
    # KonViD clip ids are 6-digit integers embedded in the filename.
    feature_rows = [
        {"key": "KoNViD_1k_video/123456_600.mp4"},
        {"key": "KoNViD_1k_video/234567_600.mp4"},
    ]
    label_rows = [
        {"src": "123456_600.mp4", "mos": 3.5, "n_ratings": 5},
        {"src": "234567_600.mp4", "mos": 4.2, "n_ratings": 7},
    ]
    features_path = tmp_path / "features_konvid.jsonl"
    labels_path = tmp_path / "labels_konvid.jsonl"
    _write_jsonl(features_path, feature_rows)
    _write_jsonl(labels_path, label_rows)

    out_path = tmp_path / "out" / "konvid_with_mos.jsonl"
    report_json = tmp_path / "report.json"
    manifest = tmp_path / "konvid_batch.json"
    manifest.write_text(
        json.dumps(
            {
                "defaults": {
                    "min_match_rate": 1.0,
                    "key_normalize": "basename",
                    "feature_key_regex": "([0-9]{6,})",
                    "label_key_regex": "([0-9]{6,})",
                },
                "tables": [
                    {
                        "id": "konvid-1k",
                        "features": str(features_path),
                        "labels": [str(labels_path)],
                        "feature_key_column": "key",
                        "label_key_column": "src",
                        "out": str(out_path),
                    },
                ],
            }
        ),
        encoding="utf-8",
    )

    rc = module.main(["--manifest", str(manifest), "--report-json", str(report_json)])
    assert rc == 0, f"batch runner exited {rc}"

    out_rows = [
        json.loads(line) for line in out_path.read_text(encoding="utf-8").splitlines() if line
    ]
    assert len(out_rows) == 2
    assert out_rows[0]["mos"] == pytest.approx(3.5)
    assert out_rows[1]["mos"] == pytest.approx(4.2)
    assert all(r["mos_label_status"] == "ok" for r in out_rows)

    report = json.loads(report_json.read_text(encoding="utf-8"))
    assert report["schema"] == "mos-label-materializer-batch-v1"
    assert report["status"] == "ok"
    assert report["summary"]["matched_rows"] == 2
    assert report["summary"]["failed_tables"] == 0


# ---------------------------------------------------------------------------
# Synthetic end-to-end run — CHUG shape
# ---------------------------------------------------------------------------


def test_chug_batch_synthetic_run(tmp_path: Path) -> None:
    """End-to-end batch run using synthetic CHUG-shaped data.

    CHUG video IDs are opaque strings (not file paths), so the manifest
    uses ``key_normalize: raw`` and the same column on both sides.
    Verifies the batch runner joins on raw-string keys and emits ``mos``.
    """
    module = _load_module()

    feature_rows = [
        {"chug_video_id": "chug-clip-001", "adm2": 0.95},
        {"chug_video_id": "chug-clip-002", "adm2": 0.88},
    ]
    label_rows = [
        {"chug_video_id": "chug-clip-001", "mos": 3.8},
        {"chug_video_id": "chug-clip-002", "mos": 2.9},
    ]
    features_path = tmp_path / "chug_features.jsonl"
    labels_path = tmp_path / "chug_labels.jsonl"
    _write_jsonl(features_path, feature_rows)
    _write_jsonl(labels_path, label_rows)

    out_path = tmp_path / "out" / "chug_with_mos.jsonl"
    report_json = tmp_path / "chug_report.json"
    manifest = tmp_path / "chug_batch.json"
    manifest.write_text(
        json.dumps(
            {
                "defaults": {
                    "min_match_rate": 1.0,
                    "key_normalize": "raw",
                },
                "tables": [
                    {
                        "id": "chug-hdr",
                        "features": str(features_path),
                        "labels": [str(labels_path)],
                        "feature_key_column": "chug_video_id",
                        "label_key_column": "chug_video_id",
                        "out": str(out_path),
                    },
                ],
            }
        ),
        encoding="utf-8",
    )

    rc = module.main(["--manifest", str(manifest), "--report-json", str(report_json)])
    assert rc == 0, f"batch runner exited {rc}"

    out_rows = [
        json.loads(line) for line in out_path.read_text(encoding="utf-8").splitlines() if line
    ]
    assert len(out_rows) == 2
    mos_map = {r["chug_video_id"]: r["mos"] for r in out_rows}
    assert mos_map["chug-clip-001"] == pytest.approx(3.8)
    assert mos_map["chug-clip-002"] == pytest.approx(2.9)
    assert all(r["mos_label_status"] == "ok" for r in out_rows)

    report = json.loads(report_json.read_text(encoding="utf-8"))
    assert report["schema"] == "mos-label-materializer-batch-v1"
    assert report["status"] == "ok"
    assert report["summary"]["matched_rows"] == 2
