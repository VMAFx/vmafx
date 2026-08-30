# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for ``ai/scripts/batch_materialize_saliency_features.py``."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "batch_materialize_saliency_features.py"


def _load_module():
    spec = importlib.util.spec_from_file_location(
        "batch_materialize_saliency_features", _SCRIPT_PATH
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _write_jsonl(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows),
        encoding="utf-8",
    )


def _read_jsonl(path: Path) -> list[dict[str, object]]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def test_batch_manifest_materializes_multiple_skipped_tables(tmp_path: Path) -> None:
    module = _load_module()
    first = tmp_path / "input-a.jsonl"
    second = tmp_path / "input-b.jsonl"
    _write_jsonl(first, [{"src": "missing-a.mp4", "saliency_mean": 0.2, "saliency_var": 0.01}])
    _write_jsonl(second, [{"src": "missing-b.mp4", "saliency_mean": 0.4, "saliency_var": 0.02}])
    manifest = tmp_path / "batch.json"
    manifest.write_text(
        json.dumps(
            {
                "defaults": {"model_id": "saliency_student_v1", "max_frames": 2},
                "tables": [
                    {
                        "id": "first",
                        "input": "input-a.jsonl",
                        "output": "out/a.jsonl",
                        "audit_json": "audit/a.json",
                    },
                    {
                        "id": "second",
                        "input": "input-b.jsonl",
                        "output": "out/b.jsonl",
                        "audit_json": "audit/b.json",
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

    assert _read_jsonl(tmp_path / "out/a.jsonl")[0]["saliency_status"] == "skipped-existing"
    assert _read_jsonl(tmp_path / "out/b.jsonl")[0]["saliency_status"] == "skipped-existing"
    audit = json.loads((tmp_path / "audit/a.json").read_text(encoding="utf-8"))
    assert audit["schema"] == "saliency-materializer-audit-v1"
    assert audit["table_id"] == "first"
    report = json.loads(report_json.read_text(encoding="utf-8"))
    assert report["schema"] == "saliency-materializer-batch-v1"
    assert report["status"] == "ok"
    assert report["summary"]["tables"] == 2
    assert report["summary"]["skipped_existing"] == 2
    assert report["run_provenance"]["entrypoint"]["path"] == (
        "ai/scripts/batch_materialize_saliency_features.py"
    )
    assert "| first | ok | 1 | 0 | 1 | 0 |" in report_md.read_text(encoding="utf-8")


def test_batch_manifest_reports_row_failures(tmp_path: Path) -> None:
    module = _load_module()
    source = tmp_path / "input.jsonl"
    _write_jsonl(source, [{"src": "missing.mp4"}])
    manifest = tmp_path / "batch.json"
    manifest.write_text(
        json.dumps({"tables": [{"id": "broken", "input": "input.jsonl", "output": "out.jsonl"}]}),
        encoding="utf-8",
    )
    report_json = tmp_path / "report.json"

    assert module.main(["--manifest", str(manifest), "--report-json", str(report_json)]) == 1

    report = json.loads(report_json.read_text(encoding="utf-8"))
    assert report["status"] == "failed"
    assert report["summary"]["failed"] == 1
    assert report["tables"][0]["status"] == "row-failures"
    assert _read_jsonl(tmp_path / "out.jsonl")[0]["saliency_status"] == "missing-source"


def test_batch_manifest_can_allow_row_failures(tmp_path: Path) -> None:
    module = _load_module()
    source = tmp_path / "input.jsonl"
    _write_jsonl(source, [{"src": "missing.mp4"}])
    manifest = tmp_path / "batch.json"
    manifest.write_text(
        json.dumps({"tables": [{"id": "broken", "input": "input.jsonl", "output": "out.jsonl"}]}),
        encoding="utf-8",
    )
    report_json = tmp_path / "report.json"

    assert (
        module.main(
            [
                "--manifest",
                str(manifest),
                "--report-json",
                str(report_json),
                "--allow-row-failures",
            ]
        )
        == 0
    )

    report = json.loads(report_json.read_text(encoding="utf-8"))
    assert report["status"] == "row-failures"


def test_batch_manifest_rejects_unknown_keys(tmp_path: Path) -> None:
    module = _load_module()
    manifest = tmp_path / "batch.json"
    manifest.write_text(
        json.dumps({"tables": [{"id": "bad", "input": "in.jsonl", "output": "out.jsonl", "x": 1}]}),
        encoding="utf-8",
    )

    assert module.main(["--manifest", str(manifest)]) == 2
