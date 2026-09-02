# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Tests for ``ai/scripts/batch_materialize_second_opinion_features.py``."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "batch_materialize_second_opinion_features.py"


def _load_module():
    spec = importlib.util.spec_from_file_location(
        "batch_materialize_second_opinion_features", _SCRIPT
    )
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


def test_batch_manifest_joins_multiple_tables(tmp_path: Path) -> None:
    module = _load_module()
    _write_jsonl(tmp_path / "features-a.jsonl", [{"video_id": "a"}, {"video_id": "b"}])
    _write_jsonl(tmp_path / "features-b.jsonl", [{"video_id": "c"}])
    _write_jsonl(
        tmp_path / "scores-a.jsonl",
        [
            {"video_id": "a", "competitor": "fork-nr", "score": 71.0},
            {"video_id": "b", "competitor": "fork-nr", "score": 72.0},
        ],
    )
    _write_jsonl(
        tmp_path / "scores-b.jsonl", [{"video_id": "c", "competitor": "fork-nr", "score": 73.0}]
    )
    manifest = tmp_path / "batch.json"
    manifest.write_text(
        json.dumps(
            {
                "defaults": {"missing_policy": "fail"},
                "tables": [
                    {
                        "id": "first",
                        "features": "features-a.jsonl",
                        "scores": ["fork-nr=scores-a.jsonl"],
                        "out": "out/a.jsonl",
                        "audit_json": "audit/a.json",
                    },
                    {
                        "id": "second",
                        "features": "features-b.jsonl",
                        "scores": ["fork-nr=scores-b.jsonl"],
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
    assert first_rows[0]["second_opinion_fork_nr_score"] == 71.0
    audit = json.loads((tmp_path / "audit/a.json").read_text(encoding="utf-8"))
    assert audit["run_provenance"]["entrypoint"]["path"] == (
        "ai/scripts/batch_materialize_second_opinion_features.py"
    )
    report = json.loads(report_json.read_text(encoding="utf-8"))
    assert report["schema"] == "second-opinion-materializer-batch-v1"
    assert report["summary"]["tables"] == 2
    assert report["summary"]["input_rows"] == 3
    assert "| first | ok | 2 | 2 |" in report_md.read_text(encoding="utf-8")


def test_batch_manifest_reports_table_errors(tmp_path: Path) -> None:
    module = _load_module()
    _write_jsonl(tmp_path / "features.jsonl", [{"video_id": "a"}, {"video_id": "missing"}])
    _write_jsonl(
        tmp_path / "scores.jsonl", [{"video_id": "a", "competitor": "fork-nr", "score": 71.0}]
    )
    manifest = tmp_path / "batch.json"
    manifest.write_text(
        json.dumps(
            {
                "defaults": {"missing_policy": "fail"},
                "tables": [
                    {
                        "id": "broken",
                        "features": "features.jsonl",
                        "scores": ["fork-nr=scores.jsonl"],
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
    assert "missing second-opinion scores" in report["tables"][0]["error"]


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
                        "scores": ["scores.jsonl"],
                        "out": "out.jsonl",
                        "unknown": True,
                    }
                ]
            }
        ),
        encoding="utf-8",
    )

    assert module.main(["--manifest", str(manifest)]) == 2
