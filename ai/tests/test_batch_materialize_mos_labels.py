# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Tests for ``ai/scripts/batch_materialize_mos_labels.py``."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPTS_DIR = _REPO_ROOT / "ai" / "scripts"
_SCRIPT = _SCRIPTS_DIR / "batch_materialize_mos_labels.py"


def _load_module():
    # ``_script_bootstrap`` lives in ai/scripts/; make it importable before
    # executing the script module so the bootstrap import does not fail when
    # pytest is invoked from the repo root without PYTHONPATH=ai/scripts.
    if str(_SCRIPTS_DIR) not in sys.path:
        sys.path.insert(0, str(_SCRIPTS_DIR))
    spec = importlib.util.spec_from_file_location("batch_materialize_mos_labels", _SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _write_jsonl(path: Path, rows: list[dict[str, object]]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "\n".join(json.dumps(row, sort_keys=True) for row in rows) + "\n", encoding="utf-8"
    )
    return path


def _read_jsonl(path: Path) -> list[dict[str, object]]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def test_batch_manifest_materializes_multiple_tables(tmp_path: Path) -> None:
    module = _load_module()
    _write_jsonl(tmp_path / "features-a.jsonl", [{"video_id": "a"}, {"video_id": "b"}])
    _write_jsonl(tmp_path / "features-b.jsonl", [{"video_id": "c"}])
    _write_jsonl(
        tmp_path / "labels-a.jsonl",
        [
            {"video_id": "a", "mos": 3.5, "n_ratings": 5},
            {"video_id": "b", "mos": 4.0, "n_ratings": 6},
        ],
    )
    _write_jsonl(tmp_path / "labels-b.jsonl", [{"video_id": "c", "mos_raw_0_100": 50.0}])
    manifest = tmp_path / "batch.json"
    manifest.write_text(
        json.dumps(
            {
                "defaults": {"min_match_rate": 1.0},
                "tables": [
                    {
                        "id": "konvid",
                        "features": "features-a.jsonl",
                        "labels": ["labels-a.jsonl"],
                        "out": "out/a.jsonl",
                        "audit_json": "audit/a.json",
                    },
                    {
                        "id": "chug",
                        "features": "features-b.jsonl",
                        "labels": ["labels-b.jsonl"],
                        "out": "out/b.jsonl",
                    },
                ],
            }
        ),
        encoding="utf-8",
    )
    report_json = tmp_path / "report.json"
    report_md = tmp_path / "report.md"

    assert (
        module.main(
            [
                "--manifest",
                str(manifest),
                "--report-json",
                str(report_json),
                "--report-md",
                str(report_md),
            ]
        )
        == 0
    )

    first_rows = _read_jsonl(tmp_path / "out/a.jsonl")
    assert first_rows[0]["mos"] == pytest.approx(3.5)
    assert first_rows[0]["mos_n_ratings"] == 5
    second_rows = _read_jsonl(tmp_path / "out/b.jsonl")
    assert second_rows[0]["mos"] == pytest.approx(3.0)
    audit = json.loads((tmp_path / "audit/a.json").read_text(encoding="utf-8"))
    assert audit["run_provenance"]["entrypoint"]["path"] == (
        "ai/scripts/batch_materialize_mos_labels.py"
    )
    report = json.loads(report_json.read_text(encoding="utf-8"))
    assert report["schema"] == "mos-label-materializer-batch-v1"
    assert report["summary"]["tables"] == 2
    assert report["summary"]["matched_rows"] == 3
    assert "| konvid | ok | 1.000 | 2 | 0 |" in report_md.read_text(encoding="utf-8")


def test_batch_manifest_reports_table_errors(tmp_path: Path) -> None:
    module = _load_module()
    _write_jsonl(tmp_path / "features.jsonl", [{"video_id": "a"}, {"video_id": "missing"}])
    _write_jsonl(tmp_path / "labels.jsonl", [{"video_id": "a", "mos": 4.0}])
    manifest = tmp_path / "batch.json"
    manifest.write_text(
        json.dumps(
            {
                "defaults": {"min_match_rate": 1.0},
                "tables": [
                    {
                        "id": "broken",
                        "features": "features.jsonl",
                        "labels": ["labels.jsonl"],
                        "out": "out.jsonl",
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    report_json = tmp_path / "report.json"

    assert module.main(["--manifest", str(manifest), "--report-json", str(report_json)]) == 1

    report = json.loads(report_json.read_text(encoding="utf-8"))
    assert report["status"] == "failed"
    assert report["summary"]["failed_tables"] == 1
    assert "match rate" in report["tables"][0]["error"]


def test_batch_manifest_rejects_unknown_keys(tmp_path: Path) -> None:
    module = _load_module()
    manifest = tmp_path / "batch.json"
    manifest.write_text(
        json.dumps(
            {
                "tables": [
                    {
                        "id": "bad",
                        "features": "features.jsonl",
                        "labels": ["labels.jsonl"],
                        "out": "out.jsonl",
                        "unknown": True,
                    }
                ]
            }
        ),
        encoding="utf-8",
    )

    assert module.main(["--manifest", str(manifest)]) == 2
