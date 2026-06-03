# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Tests for explicit MOS-label materialisation onto feature tables."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "materialize_mos_labels.py"
_SPEC = importlib.util.spec_from_file_location("materialize_mos_labels", _SCRIPT)
assert _SPEC is not None and _SPEC.loader is not None
mos_labels = importlib.util.module_from_spec(_SPEC)
sys.modules["materialize_mos_labels"] = mos_labels
_SPEC.loader.exec_module(mos_labels)


def _write_jsonl(path: Path, rows: list[dict]) -> Path:
    path.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
    return path


def _read_jsonl(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def test_materialize_joins_mos_labels_by_regex_key(tmp_path: Path) -> None:
    features = _write_jsonl(
        tmp_path / "features.jsonl",
        [
            {"key": "10008004183", "adm2_mean": 0.91},
            {"key": "10008004201", "adm2_mean": 0.87},
        ],
    )
    labels = _write_jsonl(
        tmp_path / "labels.jsonl",
        [
            {
                "src": "orig_10008004183_540_5s.mp4",
                "mos": 3.5,
                "mos_std_dev": 0.41,
                "n_ratings": 5,
            },
            {
                "src": "orig_10008004201_540_5s.mp4",
                "mos": 4.0,
                "mos_std_dev": 0.35,
                "n_ratings": 6,
            },
        ],
    )
    out = tmp_path / "features.with_mos.jsonl"

    audit = mos_labels.materialize(
        features,
        [labels],
        out,
        label_key_column="src",
        feature_key_regex=r"([0-9]{6,})",
        label_key_regex=r"([0-9]{6,})",
    )

    rows = _read_jsonl(out)
    assert rows[0]["mos"] == pytest.approx(3.5)
    assert rows[0]["mos_raw_0_100"] == pytest.approx(62.5)
    assert rows[0]["mos_label_status"] == "ok"
    assert rows[0]["mos_std_dev"] == pytest.approx(0.41)
    assert rows[0]["mos_n_ratings"] == 5
    assert audit["match_rate"] == pytest.approx(1.0)
    assert audit["matched_rows"] == 2


def test_materialize_converts_raw_0_100_mos(tmp_path: Path) -> None:
    features = _write_jsonl(tmp_path / "features.jsonl", [{"video_id": "a"}])
    labels = _write_jsonl(tmp_path / "labels.jsonl", [{"video_id": "a", "mos_raw_0_100": 50.0}])
    out = tmp_path / "features.with_mos.jsonl"

    mos_labels.materialize(features, [labels], out)

    row = _read_jsonl(out)[0]
    assert row["mos"] == pytest.approx(3.0)
    assert row["mos_raw_0_100"] == pytest.approx(50.0)


def test_materialize_rejects_low_unique_key_coverage(tmp_path: Path) -> None:
    features = _write_jsonl(
        tmp_path / "features.jsonl",
        [{"video_id": "a"}, {"video_id": "b"}, {"video_id": "c"}],
    )
    labels = _write_jsonl(tmp_path / "labels.jsonl", [{"video_id": "a", "mos": 4.0}])

    with pytest.raises(ValueError, match="match rate"):
        mos_labels.materialize(features, [labels], tmp_path / "out.jsonl", min_match_rate=0.8)


def test_materialize_can_mark_missing_when_threshold_allows_it(tmp_path: Path) -> None:
    features = _write_jsonl(
        tmp_path / "features.jsonl",
        [{"video_id": "a"}, {"video_id": "b"}],
    )
    labels = _write_jsonl(tmp_path / "labels.jsonl", [{"video_id": "a", "mos": 4.0}])
    out = tmp_path / "out.jsonl"

    audit = mos_labels.materialize(features, [labels], out, min_match_rate=0.5)

    rows = _read_jsonl(out)
    assert rows[0]["mos_label_status"] == "ok"
    assert rows[1]["mos_label_status"] == "missing-label"
    assert rows[1]["mos"] is None
    assert audit["missing_rows"] == 1


def test_materialize_rejects_conflicting_duplicate_labels(tmp_path: Path) -> None:
    features = _write_jsonl(tmp_path / "features.jsonl", [{"video_id": "a"}])
    labels = _write_jsonl(
        tmp_path / "labels.jsonl",
        [
            {"video_id": "a", "mos": 3.0},
            {"video_id": "a", "mos": 3.1},
        ],
    )

    with pytest.raises(ValueError, match="conflicting MOS labels"):
        mos_labels.materialize(features, [labels], tmp_path / "out.jsonl")


def test_materialize_requires_overwrite_for_existing_mos_columns(tmp_path: Path) -> None:
    features = _write_jsonl(tmp_path / "features.jsonl", [{"video_id": "a", "mos": 2.0}])
    labels = _write_jsonl(tmp_path / "labels.jsonl", [{"video_id": "a", "mos": 3.0}])

    with pytest.raises(ValueError, match="refusing to overwrite"):
        mos_labels.materialize(features, [labels], tmp_path / "out.jsonl")

    out = tmp_path / "out.jsonl"
    mos_labels.materialize(features, [labels], out, overwrite=True)
    assert _read_jsonl(out)[0]["mos"] == pytest.approx(3.0)


def test_materialize_parquet_roundtrip_and_audit(tmp_path: Path) -> None:
    pd = pytest.importorskip("pandas")
    features = tmp_path / "features.parquet"
    labels = tmp_path / "labels.csv"
    out = tmp_path / "features.with_mos.parquet"
    audit_json = tmp_path / "audit.json"
    pd.DataFrame([{"clip_name": "clip_a.mp4", "adm2_mean": 0.8}]).to_parquet(
        features,
        index=False,
    )
    labels.write_text("filename,mos\nclip_a.mp4,4.2\n", encoding="utf-8")

    mos_labels.materialize(
        features,
        [labels],
        out,
        audit_json=audit_json,
        feature_key_column="clip_name",
        label_key_column="filename",
    )

    frame = pd.read_parquet(out)
    assert frame.loc[0, "mos"] == pytest.approx(4.2)
    audit = json.loads(audit_json.read_text(encoding="utf-8"))
    assert audit["feature_key_column"] == "clip_name"
    assert audit["label_key_column"] == "filename"


def test_main_audit_records_run_provenance(tmp_path: Path) -> None:
    features = _write_jsonl(tmp_path / "features.jsonl", [{"video_id": "a"}])
    labels = _write_jsonl(tmp_path / "labels.jsonl", [{"video_id": "a", "mos": 4.0}])
    out = tmp_path / "features.with_mos.jsonl"
    audit_json = tmp_path / "audit.json"

    assert (
        mos_labels.main(
            [
                "--features",
                str(features),
                "--labels",
                str(labels),
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
    assert provenance["entrypoint"]["path"] == "ai/scripts/materialize_mos_labels.py"
    assert provenance["inputs"]["features"]["kind"] == "file"
    assert provenance["inputs"]["labels"][0]["kind"] == "file"
    assert provenance["outputs"]["out"] == str(out)
