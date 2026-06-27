# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.aggregate_corpora` (ADR-0340).

Covers the contract the multi-corpus aggregator owes the trainer:

* per-corpus scale-conversion math (1-5 ACR -> 0-100 unified, 0-100
  Waterloo identity, VMAF identity);
* cross-corpus dedup picks the row with tighter MOS uncertainty;
* missing-input graceful degradation (some shards absent vs. all
  shards absent);
* schema-violating inputs hard-fail rather than silently producing
  a bad unified corpus;
* unknown / non-canonical corpus labels are dropped with a counter,
  not silently re-mapped.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_AGG_PATH = _REPO_ROOT / "ai" / "scripts" / "aggregate_corpora.py"


def _load_agg_module():
    spec = importlib.util.spec_from_file_location("aggregate_corpora", _AGG_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["aggregate_corpora"] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def agg():
    return _load_agg_module()


# ---------------------------------------------------------------------------
# Per-corpus row builders
# ---------------------------------------------------------------------------


def _likert_row(*, src: str, sha: str, mos: float, corpus: str) -> dict:
    return {
        "src": src,
        "src_sha256": sha,
        "src_size_bytes": 1024,
        "width": 1920,
        "height": 1080,
        "framerate": 24.0,
        "duration_s": 5.0,
        "pix_fmt": "yuv420p",
        "encoder_upstream": "h264",
        "mos": mos,
        "mos_std_dev": 0.5,
        "n_ratings": 30,
        "corpus": corpus,
        "corpus_version": f"{corpus}-2019",
        "ingested_at_utc": "2026-05-08T00:00:00+00:00",
    }


def _waterloo_row(*, src: str, sha: str, mos: float) -> dict:
    return {
        "src": src,
        "src_sha256": sha,
        "src_size_bytes": 1024,
        "width": 3840,
        "height": 2160,
        "framerate": 30.0,
        "duration_s": 10.0,
        "pix_fmt": "yuv420p",
        "encoder_upstream": "hevc",
        "mos": mos,  # 0-100 native
        "mos_std_dev": 4.2,
        "n_ratings": 24,
        "corpus": "waterloo-ivc-4k",
        "corpus_version": "waterloo-ivc-4k-2017",
        "ingested_at_utc": "2026-05-08T00:00:00+00:00",
    }


def _netflix_row(*, src: str, sha: str, score: float) -> dict:
    return {
        "src": src,
        "src_sha256": sha,
        "width": 1920,
        "height": 1080,
        "framerate": 24.0,
        "duration_s": 5.0,
        "pix_fmt": "yuv420p",
        "mos": score,  # already on 0-100 VMAF axis
        "mos_std_dev": 0.0,
        "n_ratings": 0,
        "corpus": "netflix-public",
        "corpus_version": "netflix-public-2018",
        "ingested_at_utc": "2026-05-08T00:00:00+00:00",
    }


def _write_jsonl(path: Path, rows: list[dict]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fp:
        for r in rows:
            fp.write(json.dumps(r, sort_keys=True) + "\n")
    return path


# ---------------------------------------------------------------------------
# Scale-conversion accuracy
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "corpus,native,expected",
    [
        ("konvid-1k", 1.0, 0.0),
        ("konvid-1k", 3.0, 50.0),
        ("konvid-1k", 5.0, 100.0),
        ("konvid-150k", 2.5, 37.5),
        ("lsvq", 4.0, 75.0),
        ("youtube-ugc", 3.4, 60.0),
        ("waterloo-ivc-4k", 0.0, 0.0),
        ("waterloo-ivc-4k", 100.0, 100.0),
        ("waterloo-ivc-4k", 73.5, 73.5),
        ("netflix-public", 95.4, 95.4),
    ],
)
def test_convert_mos_per_corpus(agg, corpus, native, expected):
    out = agg.convert_mos(native, corpus)
    assert out == pytest.approx(expected, abs=1e-9)


def test_convert_mos_unknown_corpus_raises(agg):
    with pytest.raises(ValueError, match="unknown corpus_source"):
        agg.convert_mos(3.0, "made-up-dataset")


def test_convert_mos_out_of_native_range_raises(agg):
    # 6.0 on a 1-5 ACR scale is implausible — refuse rather than
    # silently emit a 125.0 unified score.
    with pytest.raises(ValueError, match="outside the published"):
        agg.convert_mos(6.0, "konvid-1k")
    # Negative on a 0-100 scale is implausible too.
    with pytest.raises(ValueError, match="outside the published"):
        agg.convert_mos(-10.0, "waterloo-ivc-4k")


def test_convert_mos_within_slack_passes(agg):
    # Tiny float-precision overshoot must not fail.
    assert agg.convert_mos(5.001, "konvid-1k") == pytest.approx(100.025, abs=1e-9)


def test_scale_conversion_table_is_complete(agg):
    """Every label declared in the docstring must exist in the table."""
    expected_labels = {
        "konvid-1k",
        "konvid-150k",
        "lsvq",
        "youtube-ugc",
        "waterloo-ivc-4k",
        "netflix-public",
    }
    assert set(agg.SCALE_CONVERSIONS.keys()) == expected_labels


# ---------------------------------------------------------------------------
# Row transform
# ---------------------------------------------------------------------------


def test_transform_row_emits_provenance_fields(agg):
    src = _likert_row(src="a.mp4", sha="a" * 64, mos=4.0, corpus="lsvq")
    out = agg.transform_row(
        src,
        corpus_source="lsvq",
        aggregated_at_utc="2026-05-09T00:00:00+00:00",
    )
    assert out["mos"] == pytest.approx(75.0, abs=1e-9)
    assert out["mos_native"] == pytest.approx(4.0, abs=1e-9)
    assert out["mos_native_scale"] == "1-5-acr"
    assert out["corpus_source"] == "lsvq"
    assert out["corpus"] == "lsvq"
    assert out["aggregated_at_utc"] == "2026-05-09T00:00:00+00:00"
    assert out["ingested_at_utc"] == "2026-05-08T00:00:00+00:00"
    # Geometry passes through unchanged.
    assert out["width"] == 1920
    assert out["height"] == 1080


# ---------------------------------------------------------------------------
# Cross-corpus dedup
# ---------------------------------------------------------------------------


def test_resolve_duplicate_keeps_tighter_uncertainty(agg):
    a = {"corpus_source": "lsvq", "mos_std_dev": 0.8, "src_sha256": "x"}
    b = {"corpus_source": "konvid-150k", "mos_std_dev": 0.3, "src_sha256": "x"}
    keep = agg.resolve_duplicate(a, b)
    assert keep["corpus_source"] == "konvid-150k"


def test_resolve_duplicate_zero_std_loses_to_known_std(agg):
    a = {"corpus_source": "youtube-ugc", "mos_std_dev": 0.7, "src_sha256": "x"}
    b = {"corpus_source": "netflix-public", "mos_std_dev": 0.0, "src_sha256": "x"}
    keep = agg.resolve_duplicate(a, b)
    assert keep["corpus_source"] == "youtube-ugc"


def test_resolve_duplicate_tie_keeps_first_seen(agg):
    a = {"corpus_source": "lsvq", "mos_std_dev": 0.5, "src_sha256": "x"}
    b = {"corpus_source": "konvid-150k", "mos_std_dev": 0.5, "src_sha256": "x"}
    keep = agg.resolve_duplicate(a, b)
    assert keep is a


def test_aggregate_cross_corpus_dedup_end_to_end(agg, tmp_path):
    shared_sha = "deadbeef" * 8
    konvid_path = _write_jsonl(
        tmp_path / "konvid_150k.jsonl",
        [
            _likert_row(src="dup.mp4", sha=shared_sha, mos=3.6, corpus="konvid-150k"),
            _likert_row(src="k_only.mp4", sha="11" * 32, mos=4.5, corpus="konvid-150k"),
        ],
    )
    lsvq_path = _write_jsonl(
        tmp_path / "lsvq.jsonl",
        [
            _likert_row(src="dup.mp4", sha=shared_sha, mos=3.4, corpus="lsvq"),
            _likert_row(src="l_only.mp4", sha="22" * 32, mos=2.0, corpus="lsvq"),
        ],
    )
    # Konvid row has tighter std_dev -> 0.5; lsvq is 0.5 too. Force
    # lsvq to be wider so the dedup outcome is deterministic and the
    # test is asserting on uncertainty, not arrival order.
    lsvq_rows = [json.loads(ln) for ln in lsvq_path.read_text().splitlines()]
    lsvq_rows[0]["mos_std_dev"] = 0.9
    _write_jsonl(lsvq_path, lsvq_rows)

    out_path = tmp_path / "unified.jsonl"
    counters = agg.aggregate(
        [konvid_path, lsvq_path],
        out_path,
        now_fn=lambda: "2026-05-09T00:00:00+00:00",
    )

    assert counters["cross_corpus_dedups"] == 1
    assert counters["rows_out"] == 3
    assert counters["inputs_seen"] == 2
    assert counters["inputs_missing"] == 0

    rows = [json.loads(ln) for ln in out_path.read_text().splitlines()]
    by_sha = {r["src_sha256"]: r for r in rows}
    # Shared sha resolves to konvid-150k (tighter std_dev).
    assert by_sha[shared_sha]["corpus_source"] == "konvid-150k"
    assert by_sha[shared_sha]["mos"] == pytest.approx(65.0, abs=1e-9)


# ---------------------------------------------------------------------------
# Graceful degradation: missing inputs
# ---------------------------------------------------------------------------


def test_partial_input_missing_warns_but_succeeds(agg, tmp_path, caplog):
    konvid_path = _write_jsonl(
        tmp_path / "konvid_150k.jsonl",
        [_likert_row(src="a.mp4", sha="a" * 64, mos=3.0, corpus="konvid-150k")],
    )
    missing_path = tmp_path / "lsvq.jsonl"  # never created

    out_path = tmp_path / "unified.jsonl"
    with caplog.at_level("WARNING"):
        counters = agg.aggregate(
            [konvid_path, missing_path],
            out_path,
            now_fn=lambda: "2026-05-09T00:00:00+00:00",
        )
    assert counters["inputs_seen"] == 1
    assert counters["inputs_missing"] == 1
    assert counters["rows_out"] == 1
    assert any("input not found" in r.getMessage() for r in caplog.records)


def test_all_inputs_missing_aborts(agg, tmp_path):
    out_path = tmp_path / "unified.jsonl"
    with pytest.raises(SystemExit) as excinfo:
        agg.aggregate(
            [tmp_path / "missing_a.jsonl", tmp_path / "missing_b.jsonl"],
            out_path,
        )
    assert "no input JSONL files exist" in str(excinfo.value)
    assert not out_path.exists()


# ---------------------------------------------------------------------------
# Per-corpus run (single shard, useful for partial pipelines)
# ---------------------------------------------------------------------------


def test_single_shard_run_works(agg, tmp_path):
    waterloo_path = _write_jsonl(
        tmp_path / "waterloo.jsonl",
        [
            _waterloo_row(src="w1.mp4", sha="33" * 32, mos=72.0),
            _waterloo_row(src="w2.mp4", sha="44" * 32, mos=88.5),
        ],
    )
    out_path = tmp_path / "unified.jsonl"
    counters = agg.aggregate(
        [waterloo_path],
        out_path,
        now_fn=lambda: "2026-05-09T00:00:00+00:00",
    )
    assert counters["rows_out"] == 2
    rows = [json.loads(ln) for ln in out_path.read_text().splitlines()]
    assert all(r["corpus_source"] == "waterloo-ivc-4k" for r in rows)
    assert sorted(r["mos"] for r in rows) == pytest.approx([72.0, 88.5])


# ---------------------------------------------------------------------------
# Schema validation (hard-fail on missing required keys)
# ---------------------------------------------------------------------------


def test_missing_required_key_aborts(agg, tmp_path):
    bad_row = _likert_row(src="x.mp4", sha="55" * 32, mos=3.0, corpus="lsvq")
    bad_row.pop("src_sha256")
    bad_path = _write_jsonl(tmp_path / "bad.jsonl", [bad_row])
    out_path = tmp_path / "unified.jsonl"
    with pytest.raises(SystemExit) as excinfo:
        agg.aggregate(
            [bad_path],
            out_path,
            now_fn=lambda: "2026-05-09T00:00:00+00:00",
        )
    assert "missing required keys" in str(excinfo.value)


def test_invalid_json_aborts(agg, tmp_path):
    bad_path = tmp_path / "bad.jsonl"
    bad_path.write_text('{"src": "a", "mos": 3.0\n')  # missing closing brace
    out_path = tmp_path / "unified.jsonl"
    with pytest.raises(SystemExit):
        agg.aggregate([bad_path], out_path)


# ---------------------------------------------------------------------------
# Unknown / non-canonical corpus label
# ---------------------------------------------------------------------------


def test_unknown_corpus_label_drops_row(agg, tmp_path, caplog):
    odd_row = _likert_row(src="o.mp4", sha="66" * 32, mos=3.0, corpus="weird-set")
    odd_path = _write_jsonl(tmp_path / "weird.jsonl", [odd_row])
    out_path = tmp_path / "unified.jsonl"
    with caplog.at_level("WARNING"):
        counters = agg.aggregate(
            [odd_path],
            out_path,
            now_fn=lambda: "2026-05-09T00:00:00+00:00",
        )
    assert counters["rows_in"] == 1
    assert counters["rows_out"] == 0
    assert counters["dropped_unknown_corpus"] == 1
    # And critically: the unified file is empty rather than carrying
    # a guessed-conversion row that would silently widen the
    # training-target distribution.
    assert out_path.read_text() == ""


@pytest.mark.parametrize("bad_mos", [None, "n/a", [1, 2]])
def test_malformed_mos_drops_row_not_crash(agg, tmp_path, caplog, bad_mos):
    # A null / non-numeric mos cell satisfies the required-key schema check
    # (the key is present) but float() raises TypeError, not ValueError.
    # The aggregator must count it as dropped_bad_scale and keep going,
    # not abort the entire multi-corpus run on one malformed row.
    good = _likert_row(src="g.mp4", sha="ab" * 32, mos=4.0, corpus="lsvq")
    bad = _likert_row(src="b.mp4", sha="cd" * 32, mos=3.0, corpus="lsvq")
    bad["mos"] = bad_mos
    path = _write_jsonl(tmp_path / "mix.jsonl", [bad, good])
    out_path = tmp_path / "unified.jsonl"
    with caplog.at_level("WARNING"):
        counters = agg.aggregate(
            [path],
            out_path,
            now_fn=lambda: "2026-05-09T00:00:00+00:00",
        )
    assert counters["rows_in"] == 2
    assert counters["dropped_bad_scale"] == 1
    assert counters["rows_out"] == 1
    rows = [json.loads(ln) for ln in out_path.read_text().splitlines()]
    assert len(rows) == 1
    assert rows[0]["src"] == "g.mp4"


def test_corpus_source_override_wins_over_row_label(agg, tmp_path):
    odd_row = _likert_row(src="o.mp4", sha="77" * 32, mos=4.0, corpus="weird-set")
    odd_path = _write_jsonl(tmp_path / "weird.jsonl", [odd_row])
    out_path = tmp_path / "unified.jsonl"
    counters = agg.aggregate(
        [odd_path],
        out_path,
        corpus_source_overrides={odd_path: "lsvq"},
        now_fn=lambda: "2026-05-09T00:00:00+00:00",
    )
    assert counters["rows_out"] == 1
    rows = [json.loads(ln) for ln in out_path.read_text().splitlines()]
    assert rows[0]["corpus_source"] == "lsvq"
    assert rows[0]["mos"] == pytest.approx(75.0, abs=1e-9)


# ---------------------------------------------------------------------------
# Determinism: stable byte-output for a fixed input set
# ---------------------------------------------------------------------------


def test_output_is_deterministic_across_runs(agg, tmp_path):
    rows = [
        _likert_row(src="a.mp4", sha="aa" * 32, mos=3.0, corpus="konvid-150k"),
        _likert_row(src="b.mp4", sha="bb" * 32, mos=4.0, corpus="lsvq"),
        _waterloo_row(src="c.mp4", sha="cc" * 32, mos=70.0),
    ]
    konvid_p = _write_jsonl(tmp_path / "konvid.jsonl", [rows[0]])
    lsvq_p = _write_jsonl(tmp_path / "lsvq.jsonl", [rows[1]])
    waterloo_p = _write_jsonl(tmp_path / "waterloo.jsonl", [rows[2]])

    out_a = tmp_path / "a.jsonl"
    out_b = tmp_path / "b.jsonl"
    agg.aggregate([konvid_p, lsvq_p, waterloo_p], out_a, now_fn=lambda: "2026-05-09T00:00:00+00:00")
    # Different input order, same content set.
    agg.aggregate([waterloo_p, konvid_p, lsvq_p], out_b, now_fn=lambda: "2026-05-09T00:00:00+00:00")
    assert out_a.read_text() == out_b.read_text()


# ---------------------------------------------------------------------------
# CLI override parser
# ---------------------------------------------------------------------------


def test_parse_overrides_rejects_bad_label(agg):
    with pytest.raises(SystemExit, match="unknown corpus-source label"):
        agg._parse_overrides(["foo.jsonl=not-real"])


def test_parse_overrides_rejects_no_equals(agg):
    with pytest.raises(SystemExit, match="PATH=LABEL"):
        agg._parse_overrides(["foo.jsonl"])


# ---------------------------------------------------------------------------
# Manifest sidecar
# ---------------------------------------------------------------------------


def test_cli_writes_default_manifest(agg, tmp_path):
    source_path = _write_jsonl(
        tmp_path / "konvid.jsonl",
        [_likert_row(src="a.mp4", sha="aa" * 32, mos=4.0, corpus="konvid-150k")],
    )
    out_path = tmp_path / "unified.jsonl"

    assert agg.main(["--inputs", str(source_path), "--output", str(out_path)]) == 0

    manifest_path = out_path.with_suffix(".manifest.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["schema"] == "mos-corpus-aggregate-manifest-v1"
    assert manifest["counters"]["rows_out"] == 1
    assert manifest["scale_conversions"]["konvid-150k"]["scale_label"] == "1-5-acr"
    provenance = manifest["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["outputs"]["jsonl"]["path"].endswith("unified.jsonl")
    assert provenance["outputs"]["manifest"]["path"].endswith("unified.manifest.json")
