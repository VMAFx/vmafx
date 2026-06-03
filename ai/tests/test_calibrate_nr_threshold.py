# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for NR threshold calibration report provenance."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

# pylint: disable=wrong-import-position
import calibrate_nr_threshold


def test_fr_vmaf_uses_current_cli_pixel_format_and_quiet(monkeypatch, tmp_path: Path) -> None:
    ref = tmp_path / "ref.yuv"
    dist = tmp_path / "dist.yuv"
    ref.write_bytes(b"ref")
    dist.write_bytes(b"dist")
    captured: dict[str, Any] = {}

    class Completed:
        returncode = 0
        stdout = '{"pooled_metrics": {"vmaf": {"mean": 91.25}}}'

    def fake_run(cmd: list[str], **_kwargs: Any) -> Completed:
        captured["cmd"] = cmd
        return Completed()

    monkeypatch.setattr(calibrate_nr_threshold.subprocess, "run", fake_run)

    score = calibrate_nr_threshold._run_fr_vmaf(
        ref,
        dist,
        width=576,
        height=324,
        pix_fmt="yuv422p10le",
        vmaf_bin="vmaf-test",
    )

    assert score == 91.25
    assert captured["cmd"] == [
        "vmaf-test",
        "--reference",
        str(ref),
        "--distorted",
        str(dist),
        "--width",
        "576",
        "--height",
        "324",
        "--pixel_format",
        "422",
        "--bitdepth",
        "10",
        "--output",
        "/dev/stdout",
        "--json",
        "--quiet",
    ]


def test_find_yuv_files_prefers_reference_subdir(tmp_path: Path) -> None:
    corpus = tmp_path / "netflix"
    ref_dir = corpus / "ref"
    dis_dir = corpus / "dis"
    ref_dir.mkdir(parents=True)
    dis_dir.mkdir()
    ref = ref_dir / "BigBuckBunny_25fps.yuv"
    dis = dis_dir / "BigBuckBunny_20_288_375.yuv"
    ref.write_bytes(b"ref")
    dis.write_bytes(b"dis")

    assert calibrate_nr_threshold._find_yuv_files(corpus) == [ref]


def test_detect_yuv_geometry_knows_netflix_public_1080p_names() -> None:
    assert calibrate_nr_threshold._detect_yuv_geometry(Path("BigBuckBunny_25fps.yuv")) == (
        1920,
        1080,
    )


def test_nr_threshold_calibration_records_run_provenance(monkeypatch, tmp_path: Path) -> None:
    corpus = tmp_path / "corpus"
    corpus.mkdir()
    yuv = corpus / "sample_64x64.yuv"
    yuv.write_bytes(b"\x00" * 64)

    model_json = tmp_path / "nr_metric_v1.json"
    model_json.write_text('{"id": "nr_metric_v1"}\n', encoding="utf-8")
    model_onnx = tmp_path / "nr_metric_v1.onnx"
    model_onnx.write_bytes(b"fake-onnx")
    report_dir = tmp_path / "reports"

    def fake_encode(*_args, output: Path, **_kwargs) -> bool:
        output.write_bytes(b"encoded")
        return True

    def fake_decode(_encoded: Path, output: Path, **_kwargs) -> bool:
        output.write_bytes(b"decoded")
        return True

    def fake_fr(_ref_yuv: Path, dist_yuv: Path, **_kwargs) -> float:
        return 92.0 if "crf20" in dist_yuv.name else 82.0

    def fake_nr(dist_yuv: Path, **kwargs) -> float:
        assert kwargs["nr_use_gpu_ep"] is False
        return 90.0 if "crf20" in dist_yuv.name else 80.0

    monkeypatch.setattr(calibrate_nr_threshold, "_encode_yuv", fake_encode)
    monkeypatch.setattr(calibrate_nr_threshold, "_decode_to_yuv", fake_decode)
    monkeypatch.setattr(calibrate_nr_threshold, "_run_fr_vmaf", fake_fr)
    monkeypatch.setattr(calibrate_nr_threshold, "_run_nr_score", fake_nr)

    rc = calibrate_nr_threshold.main(
        [
            "--corpus",
            str(corpus),
            "--output",
            str(model_json),
            "--onnx",
            str(model_onnx),
            "--crfs",
            "20,30",
            "--max-clips",
            "1",
            "--report-dir",
            str(report_dir),
            "--min-calibration-samples",
            "2",
            "--nr-ep",
            "cpu",
        ]
    )

    assert rc == 0
    payload = json.loads(model_json.read_text(encoding="utf-8"))
    assert payload["calibration_slope"] == 1.0
    assert payload["calibration_intercept"] == 2.0
    assert payload["calibration_threshold"] == 0.0
    assert payload["calibration_quality_status"] == "accepted"
    assert payload["calibration_quality_reasons"] == []
    assert payload["calibration_min_samples"] == 2
    assert payload["calibration_min_plcc"] == 0.7
    assert payload["calibration_allow_weak"] is False
    provenance = payload["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/calibrate_nr_threshold.py"
    assert provenance["inputs"]["requested_corpus"]["kind"] == "directory"
    assert provenance["inputs"]["model_onnx"]["kind"] == "file"
    assert provenance["outputs"]["model_json"]["path"] == str(model_json)
    assert provenance["outputs"]["markdown_report"]["path"].startswith(str(report_dir))
    assert provenance["args"]["max_clips"] == 1
    assert provenance["args"]["nr_ep"] == "cpu"


def test_nr_threshold_single_sample_requires_delta_override(monkeypatch, tmp_path: Path) -> None:
    corpus = tmp_path / "corpus"
    corpus.mkdir()
    yuv = corpus / "sample_64x64.yuv"
    yuv.write_bytes(b"\x00" * 64)

    model_json = tmp_path / "nr_metric_v1.json"
    model_json.write_text('{"id": "nr_metric_v1"}\n', encoding="utf-8")
    model_onnx = tmp_path / "nr_metric_v1.onnx"
    model_onnx.write_bytes(b"fake-onnx")

    def fake_encode(*_args: Any, output: Path, **_kwargs: Any) -> bool:
        output.write_bytes(b"encoded")
        return True

    def fake_decode(_encoded: Path, output: Path, **_kwargs: Any) -> bool:
        output.write_bytes(b"decoded")
        return True

    monkeypatch.setattr(calibrate_nr_threshold, "_encode_yuv", fake_encode)
    monkeypatch.setattr(calibrate_nr_threshold, "_decode_to_yuv", fake_decode)
    monkeypatch.setattr(calibrate_nr_threshold, "_run_fr_vmaf", lambda *_a, **_k: 92.0)
    monkeypatch.setattr(calibrate_nr_threshold, "_run_nr_score", lambda *_a, **_k: 90.0)

    rc = calibrate_nr_threshold.main(
        [
            "--corpus",
            str(corpus),
            "--output",
            str(model_json),
            "--onnx",
            str(model_onnx),
            "--crfs",
            "20",
            "--max-clips",
            "1",
            "--report-dir",
            str(tmp_path / "reports"),
        ]
    )

    assert rc == 1
    assert "calibration_threshold" not in json.loads(model_json.read_text(encoding="utf-8"))


def test_nr_threshold_quality_gate_rejects_weak_correlation(monkeypatch, tmp_path: Path) -> None:
    corpus = tmp_path / "corpus"
    corpus.mkdir()
    yuv = corpus / "sample_64x64.yuv"
    yuv.write_bytes(b"\x00" * 64)

    model_json = tmp_path / "nr_metric_v1.json"
    model_json.write_text('{"id": "nr_metric_v1"}\n', encoding="utf-8")
    model_onnx = tmp_path / "nr_metric_v1.onnx"
    model_onnx.write_bytes(b"fake-onnx")
    report_dir = tmp_path / "reports"

    def fake_encode(*_args: Any, output: Path, **_kwargs: Any) -> bool:
        output.write_bytes(b"encoded")
        return True

    def fake_decode(_encoded: Path, output: Path, **_kwargs: Any) -> bool:
        output.write_bytes(b"decoded")
        return True

    def fake_fr(_ref_yuv: Path, dist_yuv: Path, **_kwargs: Any) -> float:
        if "crf20" in dist_yuv.name:
            return 70.0
        if "crf30" in dist_yuv.name:
            return 80.0
        return 90.0

    def fake_nr(dist_yuv: Path, **_kwargs: Any) -> float:
        if "crf20" in dist_yuv.name:
            return 90.0
        if "crf30" in dist_yuv.name:
            return 80.0
        return 70.0

    monkeypatch.setattr(calibrate_nr_threshold, "_encode_yuv", fake_encode)
    monkeypatch.setattr(calibrate_nr_threshold, "_decode_to_yuv", fake_decode)
    monkeypatch.setattr(calibrate_nr_threshold, "_run_fr_vmaf", fake_fr)
    monkeypatch.setattr(calibrate_nr_threshold, "_run_nr_score", fake_nr)

    rc = calibrate_nr_threshold.main(
        [
            "--corpus",
            str(corpus),
            "--output",
            str(model_json),
            "--onnx",
            str(model_onnx),
            "--crfs",
            "20,30,40",
            "--max-clips",
            "1",
            "--report-dir",
            str(report_dir),
            "--min-calibration-samples",
            "2",
        ]
    )

    assert rc == 1
    payload = json.loads(model_json.read_text(encoding="utf-8"))
    assert "calibration_threshold" not in payload
    report = next(report_dir.glob("nr_metric_v1-calibration-*.md"))
    text = report.read_text(encoding="utf-8")
    assert "**WEAK**" in text
    assert "PLCC -1.0000 < required 0.7000" in text


def test_nr_threshold_quality_gate_override_records_weak_status(
    monkeypatch, tmp_path: Path
) -> None:
    corpus = tmp_path / "corpus"
    corpus.mkdir()
    yuv = corpus / "sample_64x64.yuv"
    yuv.write_bytes(b"\x00" * 64)

    model_json = tmp_path / "nr_metric_v1.json"
    model_json.write_text('{"id": "nr_metric_v1"}\n', encoding="utf-8")
    model_onnx = tmp_path / "nr_metric_v1.onnx"
    model_onnx.write_bytes(b"fake-onnx")

    def fake_encode(*_args: Any, output: Path, **_kwargs: Any) -> bool:
        output.write_bytes(b"encoded")
        return True

    def fake_decode(_encoded: Path, output: Path, **_kwargs: Any) -> bool:
        output.write_bytes(b"decoded")
        return True

    def fake_fr(_ref_yuv: Path, dist_yuv: Path, **_kwargs: Any) -> float:
        return 70.0 if "crf20" in dist_yuv.name else 90.0

    def fake_nr(dist_yuv: Path, **_kwargs: Any) -> float:
        return 90.0 if "crf20" in dist_yuv.name else 70.0

    monkeypatch.setattr(calibrate_nr_threshold, "_encode_yuv", fake_encode)
    monkeypatch.setattr(calibrate_nr_threshold, "_decode_to_yuv", fake_decode)
    monkeypatch.setattr(calibrate_nr_threshold, "_run_fr_vmaf", fake_fr)
    monkeypatch.setattr(calibrate_nr_threshold, "_run_nr_score", fake_nr)

    rc = calibrate_nr_threshold.main(
        [
            "--corpus",
            str(corpus),
            "--output",
            str(model_json),
            "--onnx",
            str(model_onnx),
            "--crfs",
            "20,30",
            "--max-clips",
            "1",
            "--report-dir",
            str(tmp_path / "reports"),
            "--min-calibration-samples",
            "2",
            "--allow-weak-calibration",
        ]
    )

    assert rc == 0
    payload = json.loads(model_json.read_text(encoding="utf-8"))
    assert payload["calibration_quality_status"] == "weak"
    assert payload["calibration_allow_weak"] is True
    assert any("PLCC" in reason for reason in payload["calibration_quality_reasons"])
