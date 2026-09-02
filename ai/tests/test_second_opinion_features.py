# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Tests for the NR/MOS second-opinion feature materializer."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "materialize_second_opinion_features.py"
_SPEC = importlib.util.spec_from_file_location("materialize_second_opinion_features", _SCRIPT)
assert _SPEC is not None and _SPEC.loader is not None
second_opinion = importlib.util.module_from_spec(_SPEC)
sys.modules["materialize_second_opinion_features"] = second_opinion
_SPEC.loader.exec_module(second_opinion)


def _write_jsonl(path: Path, rows: list[dict]) -> Path:
    path.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
    return path


def _read_jsonl(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def test_materialize_joins_scores_by_video_id(tmp_path: Path) -> None:
    features = _write_jsonl(
        tmp_path / "features.jsonl",
        [
            {"video_id": "a", "vmaf": 91.0},
            {"video_id": "b", "vmaf": 82.0},
        ],
    )
    scores = _write_jsonl(
        tmp_path / "scores.jsonl",
        [
            {"video_id": "a", "competitor": "DOVER-Mobile", "score": 72.5, "runtime_ms": 3.0},
            {"video_id": "b", "competitor": "DOVER-Mobile", "score": 64.0, "runtime_ms": 4.0},
        ],
    )
    out = tmp_path / "enriched.jsonl"

    audit = second_opinion.materialize(features, [str(scores)], out)

    rows = _read_jsonl(out)
    assert rows[0]["second_opinion_dover_mobile_score"] == pytest.approx(72.5)
    assert rows[1]["second_opinion_dover_mobile_status"] == "ok"
    assert rows[1]["second_opinion_dover_mobile_runtime_ms"] == pytest.approx(4.0)
    assert audit["competitors"]["DOVER-Mobile"]["matched_rows"] == 2


def test_materialize_accepts_external_wrapper_payload(tmp_path: Path) -> None:
    features = _write_jsonl(
        tmp_path / "features.jsonl",
        [{"filename": "/corpus/clip_a.yuv", "vmaf": 88.0}],
    )
    wrapper_payload = {
        "name": "clip_a.yuv",
        "frames": [
            {"frame_idx": 0, "predicted_vmaf_or_mos": 70.0, "runtime_ms": 1.5},
            {"frame_idx": 1, "predicted_vmaf_or_mos": 74.0, "runtime_ms": 2.0},
        ],
        "summary": {
            "competitor": "fork-nr-metric",
            "plcc": 0.0,
            "srocc": 0.0,
            "rmse": 0.0,
            "runtime_total_ms": 3.5,
            "params": 10,
            "gflops": 0.1,
        },
    }
    scores = tmp_path / "wrapper.json"
    scores.write_text(json.dumps(wrapper_payload), encoding="utf-8")
    out = tmp_path / "enriched.jsonl"

    second_opinion.materialize(features, [str(scores)], out)

    row = _read_jsonl(out)[0]
    assert row["second_opinion_fork_nr_metric_score"] == pytest.approx(72.0)
    assert row["second_opinion_fork_nr_metric_frames"] == 2
    assert row["second_opinion_fork_nr_metric_runtime_ms"] == pytest.approx(3.5)


def test_materialize_marks_missing_scores_and_writes_audit(tmp_path: Path) -> None:
    features = _write_jsonl(
        tmp_path / "features.jsonl",
        [{"video_id": "a"}, {"video_id": "missing"}],
    )
    scores = _write_jsonl(
        tmp_path / "scores.jsonl",
        [{"video_id": "a", "competitor": "q-align", "score": 83.0}],
    )
    out = tmp_path / "enriched.jsonl"
    audit_json = tmp_path / "audit.json"

    second_opinion.materialize(features, [str(scores)], out, audit_json=audit_json)

    rows = _read_jsonl(out)
    assert rows[1]["second_opinion_q_align_score"] is None
    assert rows[1]["second_opinion_q_align_status"] == "missing"
    audit = json.loads(audit_json.read_text(encoding="utf-8"))
    assert audit["competitors"]["q-align"]["missing_rows"] == 1


def test_missing_policy_fail_rejects_incomplete_join(tmp_path: Path) -> None:
    features = _write_jsonl(
        tmp_path / "features.jsonl",
        [{"video_id": "a"}, {"video_id": "missing"}],
    )
    scores = _write_jsonl(
        tmp_path / "scores.jsonl",
        [{"video_id": "a", "competitor": "fast-vqa", "score": 83.0}],
    )

    with pytest.raises(ValueError, match="missing second-opinion scores"):
        second_opinion.materialize(
            features,
            [str(scores)],
            tmp_path / "enriched.jsonl",
            missing_policy="fail",
        )


def test_duplicate_scores_are_rejected(tmp_path: Path) -> None:
    features = _write_jsonl(tmp_path / "features.jsonl", [{"video_id": "a"}])
    scores = _write_jsonl(
        tmp_path / "scores.jsonl",
        [
            {"video_id": "a", "competitor": "musiq", "score": 70.0},
            {"video_id": "a", "competitor": "musiq", "score": 71.0},
        ],
    )

    with pytest.raises(ValueError, match="duplicate score"):
        second_opinion.materialize(features, [str(scores)], tmp_path / "out.jsonl")


def test_parquet_roundtrip_when_pandas_stack_available(tmp_path: Path) -> None:
    pd = pytest.importorskip("pandas")
    features = tmp_path / "features.parquet"
    out = tmp_path / "enriched.parquet"
    pd.DataFrame([{"video_id": "a", "vmaf": 90.0}]).to_parquet(features, index=False)
    scores = _write_jsonl(
        tmp_path / "scores.jsonl",
        [{"video_id": "a", "competitor": "clip-iqa", "score": 79.0}],
    )

    second_opinion.materialize(features, [str(scores)], out)

    frame = pd.read_parquet(out)
    assert frame.loc[0, "second_opinion_clip_iqa_score"] == pytest.approx(79.0)


def test_main_audit_records_run_provenance(tmp_path: Path) -> None:
    features = _write_jsonl(tmp_path / "features.jsonl", [{"video_id": "a"}])
    scores = _write_jsonl(
        tmp_path / "scores.jsonl",
        [{"video_id": "a", "competitor": "q-align", "score": 83.0}],
    )
    out = tmp_path / "enriched.jsonl"
    audit_json = tmp_path / "audit.json"

    assert (
        second_opinion.main(
            [
                "--features",
                str(features),
                "--scores",
                str(scores),
                "--out",
                str(out),
                "--audit-json",
                str(audit_json),
            ]
        )
        == 0
    )

    audit = json.loads(audit_json.read_text(encoding="utf-8"))
    provenance = audit["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/materialize_second_opinion_features.py"
    assert provenance["inputs"]["features"]["kind"] == "file"
    assert provenance["outputs"]["out"] == str(out)
