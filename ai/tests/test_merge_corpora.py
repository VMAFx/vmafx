# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.merge_corpora` (ADR-0310).

Covers the contract the BVI-DVC ingestion path depends on:

* concatenating two valid corpus shards de-duplicates by the
  ``(src_sha256, encoder, preset, crf)`` natural key;
* a row missing any required :data:`CORPUS_ROW_KEYS` field aborts the
  merge with ``SystemExit(1)`` rather than silently writing a
  schema-bad output the trainer would later mis-parse.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_VMAFTUNE_SRC = _REPO_ROOT / "tools" / "vmaf-tune" / "src"
if str(_VMAFTUNE_SRC) not in sys.path:
    sys.path.insert(0, str(_VMAFTUNE_SRC))

from vmaftune import CORPUS_ROW_KEYS  # noqa: E402

_MERGE_PATH = _REPO_ROOT / "ai" / "scripts" / "merge_corpora.py"


def _load_merge_module():
    spec = importlib.util.spec_from_file_location("merge_corpora", _MERGE_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _fixture_row(idx: int, *, src_sha: str | None = None) -> dict:
    """Build a minimal :data:`CORPUS_ROW_KEYS`-complete row."""
    base = {
        "schema_version": 2,
        "run_id": f"run{idx:04x}",
        "timestamp": "2026-05-05T00:00:00Z",
        "src": f"fixtures/src_{idx}.yuv",
        "src_sha256": src_sha if src_sha is not None else f"{idx:064x}",
        "width": 1920,
        "height": 1080,
        "pix_fmt": "yuv420p",
        "framerate": 24.0,
        "duration_s": 5.0,
        "encoder": "libx264",
        "encoder_version": "0.164",
        "preset": "medium",
        "crf": 23 + (idx % 4),
        "extra_params": "",
        "encode_path": f"fixtures/out_{idx}.mkv",
        "encode_size_bytes": 12345,
        "bitrate_kbps": 1234.5,
        "encode_time_ms": 100,
        "vmaf_score": 90.0 + idx * 0.1,
        "vmaf_model": "vmaf_v0.6.1",
        "score_time_ms": 50,
        "ffmpeg_version": "n8.1",
        "vmaf_binary_version": "v3.0.0-lusoris.1",
        "exit_status": 0,
        "clip_mode": "full",
        # HDR metadata trio (ADR-0223): defaults for SDR sources.
        "hdr_transfer": "bt709",
        "hdr_primaries": "bt709",
        "hdr_forced": False,
        # TransNet-V2 shot-metadata trio (ADR-0223 / research-0086):
        # `shot_count == 0` flags "shot detection unavailable" so
        # downstream consumers can opt out without a schema bump.
        "shot_count": 0,
        "shot_avg_duration_sec": 0.0,
        "shot_duration_std_sec": 0.0,
        # CANONICAL6_AGGREGATE_KEYS: canonical-6 per-feature mean+std
        # aggregates. Zeros are fine for fixture rows.
        "vif_scale0_mean": 0.0,
        "vif_scale1_mean": 0.0,
        "vif_scale2_mean": 0.0,
        "vif_scale3_mean": 0.0,
        "motion2_mean": 0.0,
        "adm2_mean": 0.0,
        "vif_scale0_std": 0.0,
        "vif_scale1_std": 0.0,
        "vif_scale2_std": 0.0,
        "vif_scale3_std": 0.0,
        "motion2_std": 0.0,
        "adm2_std": 0.0,
        # ADR-0332: per-frame encoder-internal stats aggregates.
        # Default zeros for hardware/non-internal-stats codecs.
        "enc_internal_qp_mean": 0.0,
        "enc_internal_qp_std": 0.0,
        "enc_internal_bits_mean": 0.0,
        "enc_internal_bits_std": 0.0,
        "enc_internal_mv_mean": 0.0,
        "enc_internal_mv_std": 0.0,
        "enc_internal_itex_mean": 0.0,
        "enc_internal_ptex_mean": 0.0,
        "enc_internal_intra_ratio": 0.0,
        "enc_internal_skip_ratio": 0.0,
    }
    # Sanity: every fixture row must satisfy the schema we test against.
    assert set(base.keys()) >= set(CORPUS_ROW_KEYS)
    return base


def _write_jsonl(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fp:
        for row in rows:
            fp.write(json.dumps(row, sort_keys=True) + "\n")


def test_merge_dedupes_by_natural_key(tmp_path: Path) -> None:
    """5 + 5 rows with one cross-shard duplicate → 9 rows out, 1 dupe."""
    merge_mod = _load_merge_module()

    shard_a = [_fixture_row(i) for i in range(5)]
    # Shard B starts at idx=4 so its first row collides with shard A's
    # last row on (src_sha256, encoder, preset, crf).
    shard_b = [_fixture_row(i) for i in range(4, 9)]

    a_path = tmp_path / "a.jsonl"
    b_path = tmp_path / "b.jsonl"
    out_path = tmp_path / "merged.jsonl"
    _write_jsonl(a_path, shard_a)
    _write_jsonl(b_path, shard_b)

    rows_in, rows_out, dupes, sources = merge_mod.merge([a_path, b_path], out_path)

    assert rows_in == 10
    assert rows_out == 9
    assert dupes == 1
    assert sources == 9

    written = [json.loads(ln) for ln in out_path.read_text().splitlines() if ln]
    assert len(written) == 9
    # Every written row carries the full schema.
    for row in written:
        assert set(row.keys()) >= set(CORPUS_ROW_KEYS)


def test_merge_rejects_schema_violation(tmp_path: Path) -> None:
    """A row with a missing required key triggers SystemExit(1)."""
    merge_mod = _load_merge_module()

    good = _fixture_row(0)
    bad = _fixture_row(1)
    del bad["src_sha256"]  # canonical-key field — must reject

    a_path = tmp_path / "good.jsonl"
    b_path = tmp_path / "bad.jsonl"
    out_path = tmp_path / "merged.jsonl"
    _write_jsonl(a_path, [good])
    _write_jsonl(b_path, [bad])

    with pytest.raises(SystemExit) as exc:
        merge_mod.merge([a_path, b_path], out_path)
    assert exc.value.code == 1


def test_merge_missing_input_exits_2(tmp_path: Path) -> None:
    merge_mod = _load_merge_module()
    out_path = tmp_path / "merged.jsonl"
    with pytest.raises(SystemExit) as exc:
        merge_mod.merge(
            [tmp_path / "does_not_exist.jsonl", tmp_path / "also_no.jsonl"],
            out_path,
        )
    assert exc.value.code == 2


def test_cli_writes_default_manifest(tmp_path: Path) -> None:
    merge_mod = _load_merge_module()
    a_path = tmp_path / "a.jsonl"
    b_path = tmp_path / "b.jsonl"
    out_path = tmp_path / "merged.jsonl"
    _write_jsonl(a_path, [_fixture_row(0)])
    _write_jsonl(b_path, [_fixture_row(1)])

    assert merge_mod.main(["--inputs", str(a_path), str(b_path), "--output", str(out_path)]) == 0

    manifest_path = out_path.with_suffix(".manifest.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["schema"] == "vmaf-tune-corpus-merge-manifest-v1"
    assert manifest["summary"] == {
        "duplicates": 0,
        "rows_in": 2,
        "rows_out": 2,
        "unique_sources": 2,
    }
    assert manifest["dedup_key"] == ["src_sha256", "encoder", "preset", "crf"]
    assert "src_sha256" in manifest["required_keys"]
    provenance = manifest["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert len(provenance["inputs"]["corpus_jsonl"]) == 2
    assert provenance["outputs"]["manifest"]["path"].endswith("merged.manifest.json")
