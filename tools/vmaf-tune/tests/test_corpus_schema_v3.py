# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""ADR-0331: corpus schema v3 (canonical-6 per-feature aggregates).

Round-trips a freshly-emitted v3 row through ``write_jsonl`` +
``read_jsonl`` and asserts the 12 new feature columns are present and
parse back as floats. Also pins the writer-side behaviour: features
libvmaf does not expose surface as ``NaN``, never ``0.0``.
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import (  # noqa: E402
    CANONICAL6_AGGREGATE_KEYS,
    CANONICAL6_FEATURES,
    CANONICAL6_MEAN_KEYS,
    CANONICAL6_STD_KEYS,
    CORPUS_ROW_KEYS,
    SCHEMA_VERSION,
)
from vmaftune.corpus import (  # noqa: E402
    CorpusJob,
    CorpusOptions,
    iter_rows,
    read_jsonl,
    write_jsonl,
)


class _FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _make_yuv(path: Path, nbytes: int = 1024) -> Path:
    path.write_bytes(b"\x80" * nbytes)
    return path


def _full_pooled_payload(vmaf: float = 92.5) -> dict:
    """Synthesise a libvmaf JSON payload with the canonical-6 features."""
    pooled: dict = {"vmaf": {"min": vmaf - 1, "max": vmaf + 1, "mean": vmaf, "stddev": 0.5}}
    means = {
        "adm2": 0.93,
        "vif_scale0": 0.78,
        "vif_scale1": 0.85,
        "vif_scale2": 0.91,
        "vif_scale3": 0.95,
        "motion2": 2.5,
    }
    stds = {
        "adm2": 0.04,
        "vif_scale0": 0.06,
        "vif_scale1": 0.05,
        "vif_scale2": 0.04,
        "vif_scale3": 0.03,
        "motion2": 1.2,
    }
    for name, mu in means.items():
        pooled[name] = {
            "min": mu - 0.1,
            "max": mu + 0.1,
            "mean": mu,
            "stddev": stds[name],
        }
    return {"pooled_metrics": pooled}


def test_schema_version_is_v3():
    assert SCHEMA_VERSION == 3


def test_canonical6_aggregate_keys_layout():
    assert len(CANONICAL6_FEATURES) == 6
    assert len(CANONICAL6_MEAN_KEYS) == 6
    assert len(CANONICAL6_STD_KEYS) == 6
    assert len(CANONICAL6_AGGREGATE_KEYS) == 12
    # _mean / _std for every canonical feature, in order.
    for feature, mk, sk in zip(
        CANONICAL6_FEATURES, CANONICAL6_MEAN_KEYS, CANONICAL6_STD_KEYS, strict=True
    ):
        assert mk == f"{feature}_mean"
        assert sk == f"{feature}_std"
    # All aggregate keys are part of the row contract.
    for key in CANONICAL6_AGGREGATE_KEYS:
        assert key in CORPUS_ROW_KEYS


def test_round_trip_v3_row_preserves_all_aggregates(tmp_path: Path):
    """A v3 row written + re-read carries every canonical-6 column."""
    src = _make_yuv(tmp_path / "ref.yuv")

    def fake_encode_run(cmd, capture_output, text, check):
        Path(cmd[-1]).write_bytes(b"\x00" * 4096)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\nx264 - core 164\n")

    def fake_score_run(cmd, capture_output, text, check):
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(_full_pooled_payload(vmaf=92.5)))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        keep_encodes=False,
        src_sha256=False,
    )
    rows = list(iter_rows(job, opts, encode_runner=fake_encode_run, score_runner=fake_score_run))
    assert len(rows) == 1
    row = rows[0]
    assert row["schema_version"] == 3
    # All 12 aggregate columns present with finite floats.
    for key in CANONICAL6_AGGREGATE_KEYS:
        assert key in row
        v = row[key]
        assert isinstance(v, float)
        assert math.isfinite(v), f"{key} should be finite when libvmaf exposes it, got {v!r}"
    # Spot-check a couple of values matched the synthesised payload.
    assert row["adm2_mean"] == pytest.approx(0.93)
    assert row["motion2_std"] == pytest.approx(1.2)

    # Round-trip via JSONL.
    out = tmp_path / "corpus_rt.jsonl"
    write_jsonl(rows, out)
    parsed = read_jsonl(out)
    assert len(parsed) == 1
    rt = parsed[0]
    for key in CANONICAL6_AGGREGATE_KEYS:
        assert rt[key] == pytest.approx(row[key])


def test_missing_features_become_nan_not_zero(tmp_path: Path):
    """libvmaf-side feature absence ⇒ NaN cell, never 0.0."""
    src = _make_yuv(tmp_path / "ref.yuv")

    def fake_encode_run(cmd, capture_output, text, check):
        Path(cmd[-1]).write_bytes(b"\x00" * 4096)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\nx264 - core 164\n")

    def fake_score_run(cmd, capture_output, text, check):
        # cambi-only model — no canonical-6 features in pooled_metrics.
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(
            json.dumps(
                {
                    "pooled_metrics": {
                        "vmaf": {"mean": 88.0, "stddev": 0.4},
                        "cambi": {"mean": 0.5, "stddev": 0.1},
                    }
                }
            )
        )
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        src_sha256=False,
        vmaf_model="vmaf_v0.6.1.neg",
    )
    rows = list(iter_rows(job, opts, encode_runner=fake_encode_run, score_runner=fake_score_run))
    assert len(rows) == 1
    row = rows[0]
    for key in CANONICAL6_AGGREGATE_KEYS:
        assert math.isnan(row[key]), f"{key} should be NaN when feature absent, got {row[key]!r}"


def test_encode_failure_yields_nan_aggregates(tmp_path: Path):
    """An encode failure skips scoring — every aggregate column is NaN."""
    src = _make_yuv(tmp_path / "ref.yuv")

    def failing_encode(cmd, capture_output, text, check):
        return _FakeCompleted(returncode=1, stderr="x264 boom")

    def never_score(*a, **kw):  # pragma: no cover
        raise AssertionError("score must not run when encode failed")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        src_sha256=False,
    )
    rows = list(iter_rows(job, opts, encode_runner=failing_encode, score_runner=never_score))
    assert len(rows) == 1
    for key in CANONICAL6_AGGREGATE_KEYS:
        assert math.isnan(rows[0][key])
