# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""ADR-0331: schema-v2 corpus rows still load on a v3 reader.

The reader fills missing canonical-6 columns with ``NaN`` and preserves
the original ``schema_version`` so callers can decide whether to skip
or include the row when training requires real per-feature data.
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import CANONICAL6_AGGREGATE_KEYS  # noqa: E402
from vmaftune.corpus import read_jsonl  # noqa: E402


def _v2_row(crf: int = 23, vmaf: float = 91.0) -> dict:
    """Emit a row matching the pre-ADR-0331 (v2) on-disk shape."""
    return {
        "schema_version": 2,
        "run_id": "deadbeef",
        "timestamp": "2026-04-01T00:00:00+00:00",
        "src": "ref.yuv",
        "src_sha256": "",
        "width": 1920,
        "height": 1080,
        "pix_fmt": "yuv420p",
        "framerate": 24.0,
        "duration_s": 10.0,
        "encoder": "libx264",
        "encoder_version": "libx264-164",
        "preset": "medium",
        "crf": crf,
        "extra_params": [],
        "encode_path": "",
        "encode_size_bytes": 4096,
        "bitrate_kbps": 3.2,
        "encode_time_ms": 100.0,
        "vmaf_score": vmaf,
        "vmaf_model": "vmaf_v0.6.1",
        "score_time_ms": 50.0,
        "ffmpeg_version": "6.1.1",
        "vmaf_binary_version": "3.0.0",
        "exit_status": 0,
        "clip_mode": "full",
    }


def test_read_v2_jsonl_does_not_crash(tmp_path: Path):
    p = tmp_path / "v2.jsonl"
    p.write_text("\n".join(json.dumps(_v2_row(c)) for c in (23, 28, 33)) + "\n")
    rows = read_jsonl(p)
    assert len(rows) == 3


def test_v2_row_gets_nan_canonical6_columns(tmp_path: Path):
    p = tmp_path / "v2.jsonl"
    p.write_text(json.dumps(_v2_row()) + "\n")
    rows = read_jsonl(p)
    assert len(rows) == 1
    row = rows[0]
    # Original schema_version preserved — caller decides what to do.
    assert row["schema_version"] == 2
    # Every v3-only column is present and NaN.
    for key in CANONICAL6_AGGREGATE_KEYS:
        assert key in row
        v = row[key]
        assert isinstance(v, float) and math.isnan(v), f"{key} should be NaN on a v2 row, got {v!r}"


def test_no_upgrade_keeps_row_bare(tmp_path: Path):
    p = tmp_path / "v2.jsonl"
    p.write_text(json.dumps(_v2_row()) + "\n")
    rows = read_jsonl(p, upgrade_to_current=False)
    row = rows[0]
    # Without the upgrade pass, the legacy row is unchanged.
    for key in CANONICAL6_AGGREGATE_KEYS:
        assert key not in row


def test_mixed_v2_and_v3_rows(tmp_path: Path):
    p = tmp_path / "mixed.jsonl"
    v3 = _v2_row()
    v3["schema_version"] = 3
    for key in CANONICAL6_AGGREGATE_KEYS:
        v3[key] = 0.5
    p.write_text(json.dumps(_v2_row()) + "\n" + json.dumps(v3) + "\n")
    rows = read_jsonl(p)
    assert len(rows) == 2
    legacy, current = rows
    assert legacy["schema_version"] == 2
    assert current["schema_version"] == 3
    # Legacy NaN; current carries the real value.
    assert math.isnan(legacy["adm2_mean"])
    assert current["adm2_mean"] == 0.5
