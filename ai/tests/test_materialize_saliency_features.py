# Copyright 2026 Lusoris and Claude (Anthropic)
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for ``ai/scripts/materialize_saliency_features.py``."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any

import pytest

np = pytest.importorskip("numpy")

from .conftest import load_ai_script  # noqa: E402


def _load_module():
    return load_ai_script("materialize_saliency_features")


def _runner(cmd: list[str], **_: Any) -> subprocess.CompletedProcess[str]:
    if cmd[0] == "ffprobe-test":
        payload = {"streams": [{"width": 4, "height": 4}]}
        return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps(payload), stderr="")
    if cmd[0] == "ffmpeg-test":
        Path(cmd[-1]).write_bytes(b"\x10" * (4 * 4 * 3 // 2))
        return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
    raise AssertionError(f"unexpected command: {cmd}")


def _saliency_fn(
    raw_path: Path,
    width: int,
    height: int,
    *,
    model_path: Path | None = None,
    frame_samples: int = 8,
    temporal_aggregator: str = "mean",
    ema_alpha: float = 0.6,
) -> np.ndarray:
    assert raw_path.is_file()
    assert (width, height) == (4, 4)
    assert model_path is None
    assert frame_samples == 1
    assert temporal_aggregator == "max"
    assert ema_alpha == pytest.approx(0.75)
    return np.asarray([[0.0, 0.5], [1.0, 0.5]], dtype=np.float32)


def test_read_write_jsonl_roundtrip(tmp_path: Path) -> None:
    module = _load_module()
    rows = [{"src": "a.mp4", "width": 4}, {"src": "b.mp4", "width": 8}]
    path = tmp_path / "features.jsonl"

    module.write_table(path, rows)

    assert module.read_table(path) == rows


def test_materialize_rows_decodes_and_writes_aggregates(tmp_path: Path) -> None:
    module = _load_module()
    source = tmp_path / "clip.mp4"
    source.write_bytes(b"not a real mp4")
    cfg = module.SaliencyMaterializeConfig(
        ffmpeg_bin="ffmpeg-test",
        ffprobe_bin="ffprobe-test",
        max_frames=1,
        frame_samples=3,
        model_id="saliency_student_v2",
        temporal_aggregator="max",
        ema_alpha=0.75,
    )

    enriched, summary = module.materialize_rows(
        [{"src": str(source), "width": 4, "height": 4}],
        cfg,
        runner=_runner,
        saliency_fn=_saliency_fn,
    )

    assert summary == module.MaterializeSummary(total=1, ok=1, skipped_existing=0, failed=0)
    assert enriched[0]["saliency_status"] == "ok"
    assert enriched[0]["saliency_mean"] == pytest.approx(0.5)
    assert enriched[0]["saliency_var"] == pytest.approx(0.125)
    assert enriched[0]["saliency_model_id"] == "saliency_student_v2"
    assert enriched[0]["saliency_aggregator"] == "max"
    assert enriched[0]["saliency_ema_alpha"] == pytest.approx(0.75)


def test_materialize_rows_probes_missing_geometry(tmp_path: Path) -> None:
    module = _load_module()
    source = tmp_path / "clip.mp4"
    source.write_bytes(b"not a real mp4")
    cfg = module.SaliencyMaterializeConfig(
        ffmpeg_bin="ffmpeg-test",
        ffprobe_bin="ffprobe-test",
        max_frames=1,
        frame_samples=1,
        temporal_aggregator="max",
        ema_alpha=0.75,
    )

    enriched, summary = module.materialize_rows(
        [{"src": str(source)}],
        cfg,
        runner=_runner,
        saliency_fn=_saliency_fn,
    )

    assert summary.ok == 1
    assert enriched[0]["saliency_status"] == "ok"
    assert enriched[0]["saliency_model_id"] == "saliency_student_v1"


def test_materialize_rows_skips_existing_saliency() -> None:
    module = _load_module()
    cfg = module.SaliencyMaterializeConfig()

    enriched, summary = module.materialize_rows(
        [{"src": "missing.mp4", "saliency_mean": 0.2, "saliency_var": 0.01}],
        cfg,
        runner=_runner,
        saliency_fn=_saliency_fn,
    )

    assert summary == module.MaterializeSummary(total=1, ok=0, skipped_existing=1, failed=0)
    assert enriched[0]["saliency_mean"] == pytest.approx(0.2)
    assert enriched[0]["saliency_var"] == pytest.approx(0.01)
    assert enriched[0]["saliency_status"] == "skipped-existing"
    assert "saliency_model_id" not in enriched[0]


def test_materialize_rows_marks_missing_source(tmp_path: Path) -> None:
    module = _load_module()
    cfg = module.SaliencyMaterializeConfig(root=tmp_path)

    enriched, summary = module.materialize_rows(
        [{"src": "missing.mp4"}],
        cfg,
        runner=_runner,
        saliency_fn=_saliency_fn,
    )

    assert summary.failed == 1
    assert enriched[0]["saliency_status"] == "missing-source"


def test_main_writes_audit_json_with_run_provenance(tmp_path: Path) -> None:
    module = _load_module()
    source = tmp_path / "clip.mp4"
    source.write_bytes(b"not a real mp4")
    input_path = tmp_path / "features.jsonl"
    output_path = tmp_path / "features.saliency.jsonl"
    audit_path = tmp_path / "audit.json"
    module.write_table(
        input_path,
        [
            {
                "src": str(source),
                "saliency_mean": 0.25,
                "saliency_var": 0.01,
            }
        ],
    )

    assert (
        module.main(
            [
                "--input",
                str(input_path),
                "--output",
                str(output_path),
                "--audit-json",
                str(audit_path),
            ]
        )
        == 0
    )

    audit = json.loads(audit_path.read_text(encoding="utf-8"))
    assert audit["summary"]["skipped_existing"] == 1
    provenance = audit["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/materialize_saliency_features.py"
    assert provenance["inputs"]["input"]["kind"] == "file"
    assert provenance["outputs"]["output"] == str(output_path)


def test_main_records_temporal_config_in_audit_json(tmp_path: Path) -> None:
    module = _load_module()
    input_path = tmp_path / "features.jsonl"
    output_path = tmp_path / "features.saliency.jsonl"
    audit_path = tmp_path / "audit.json"
    module.write_table(
        input_path,
        [{"src": "missing.mp4"}],
    )

    assert (
        module.main(
            [
                "--input",
                str(input_path),
                "--output",
                str(output_path),
                "--audit-json",
                str(audit_path),
                "--model-id",
                "u2netp_mirror_v1",
                "--temporal-aggregator",
                "ema",
                "--ema-alpha",
                "0.4",
            ]
        )
        == 1
    )

    audit = json.loads(audit_path.read_text(encoding="utf-8"))
    assert audit["config"]["model_id"] == "u2netp_mirror_v1"
    assert audit["config"]["temporal_aggregator"] == "ema"
    assert audit["config"]["ema_alpha"] == pytest.approx(0.4)
