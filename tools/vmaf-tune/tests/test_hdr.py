# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for HDR detection + codec flag dispatch (ADR-0295, Bucket #9).

Mocks ``ffprobe`` invocations so the test suite doesn't need the
binary on PATH. Real-binary integration coverage will piggyback on the
codec-adapter PRs (x265 / SVT-AV1) once those land.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.hdr import (
    HDR_MODEL_FILENAME,
    HdrInfo,
    detect_hdr,
    hdr_codec_args,
    hdr_model_name_for,
    reset_hdr_model_warning,
    select_hdr_vmaf_model,
)


class _FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _make_runner(payload: dict, *, returncode: int = 0):
    """Return a fake subprocess runner that emits ``payload`` as JSON."""

    def runner(cmd, capture_output, text, check):
        return _FakeCompleted(returncode=returncode, stdout=json.dumps(payload))

    return runner


# ---------------------------------------------------------------------------
# Detection
# ---------------------------------------------------------------------------


SDR_PAYLOAD = {
    "streams": [
        {
            "color_transfer": "bt709",
            "color_primaries": "bt709",
            "color_space": "bt709",
            "color_range": "tv",
            "pix_fmt": "yuv420p",
        }
    ]
}

PQ_HDR_PAYLOAD = {
    "streams": [
        {
            "color_transfer": "smpte2084",
            "color_primaries": "bt2020",
            "color_space": "bt2020nc",
            "color_range": "tv",
            "pix_fmt": "yuv420p10le",
            "side_data_list": [
                {
                    "side_data_type": "Mastering display metadata",
                    "red_x": "34000/50000",
                    "red_y": "16000/50000",
                    "green_x": "13250/50000",
                    "green_y": "34500/50000",
                    "blue_x": "7500/50000",
                    "blue_y": "3000/50000",
                    "white_point_x": "15635/50000",
                    "white_point_y": "16450/50000",
                    "min_luminance": "50/10000",
                    "max_luminance": "10000000/10000",
                },
                {
                    "side_data_type": "Content light level metadata",
                    "max_content": 1000,
                    "max_average": 400,
                },
            ],
        }
    ]
}

HLG_HDR_PAYLOAD = {
    "streams": [
        {
            "color_transfer": "arib-std-b67",
            "color_primaries": "bt2020",
            "color_space": "bt2020nc",
            "color_range": "tv",
            "pix_fmt": "yuv420p10le",
        }
    ]
}

MISMATCHED_PAYLOAD = {
    # PQ transfer with bt709 primaries — malformed, treat as SDR.
    "streams": [
        {
            "color_transfer": "smpte2084",
            "color_primaries": "bt709",
            "color_space": "bt709",
            "color_range": "tv",
            "pix_fmt": "yuv420p",
        }
    ]
}


def test_detect_hdr_returns_none_for_sdr(tmp_path: Path):
    src = tmp_path / "sdr.mp4"
    src.write_bytes(b"\x00")
    info = detect_hdr(src, runner=_make_runner(SDR_PAYLOAD))
    assert info is None


def test_detect_hdr_pq(tmp_path: Path):
    src = tmp_path / "pq.mp4"
    src.write_bytes(b"\x00")
    info = detect_hdr(src, runner=_make_runner(PQ_HDR_PAYLOAD))
    assert info is not None
    assert info.transfer == "pq"
    assert info.primaries == "bt2020"
    assert info.matrix == "bt2020nc"
    assert info.pix_fmt == "yuv420p10le"
    assert info.master_display is not None
    assert info.master_display.startswith("G(")
    assert info.max_cll == "1000,400"


def test_detect_hdr_hlg(tmp_path: Path):
    src = tmp_path / "hlg.mp4"
    src.write_bytes(b"\x00")
    info = detect_hdr(src, runner=_make_runner(HLG_HDR_PAYLOAD))
    assert info is not None
    assert info.transfer == "hlg"
    assert info.master_display is None  # not provided in payload
    assert info.max_cll is None


def test_detect_hdr_mismatched_primaries_returns_none(tmp_path: Path):
    src = tmp_path / "broken.mp4"
    src.write_bytes(b"\x00")
    info = detect_hdr(src, runner=_make_runner(MISMATCHED_PAYLOAD))
    assert info is None


def test_detect_hdr_missing_file_returns_none(tmp_path: Path):
    missing = tmp_path / "nope.mp4"
    info = detect_hdr(missing, runner=_make_runner(PQ_HDR_PAYLOAD))
    assert info is None


def test_detect_hdr_ffprobe_failure_returns_none(tmp_path: Path):
    src = tmp_path / "src.mp4"
    src.write_bytes(b"\x00")
    info = detect_hdr(src, runner=_make_runner({}, returncode=1))
    assert info is None


def test_detect_hdr_invalid_json_returns_none(tmp_path: Path):
    src = tmp_path / "src.mp4"
    src.write_bytes(b"\x00")

    def runner(cmd, capture_output, text, check):
        return _FakeCompleted(returncode=0, stdout="not json")

    assert detect_hdr(src, runner=runner) is None


# ---------------------------------------------------------------------------
# Codec dispatch
# ---------------------------------------------------------------------------


def _pq_info(**overrides) -> HdrInfo:
    base = {
        "transfer": "pq",
        "primaries": "bt2020",
        "matrix": "bt2020nc",
        "color_range": "tv",
        "pix_fmt": "yuv420p10le",
        "master_display": "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000000,500)",
        "max_cll": "1000,400",
    }
    base.update(overrides)
    return HdrInfo(**base)


def test_hdr_args_x264_emits_global_color_only():
    args = hdr_codec_args("libx264", _pq_info())
    assert "-color_primaries" in args
    assert "-color_trc" in args
    assert args[args.index("-color_trc") + 1] == "smpte2084"
    # x264 has no -x265-params equivalent; no codec-private payload.
    assert not any(a == "-x265-params" for a in args)


def test_hdr_args_x265_pq_includes_master_display_and_max_cll():
    args = hdr_codec_args("libx265", _pq_info())
    assert "-x265-params" in args
    payload = args[args.index("-x265-params") + 1]
    assert "colorprim=bt2020" in payload
    assert "transfer=smpte2084" in payload
    assert "colormatrix=bt2020nc" in payload
    assert "master-display=" in payload
    assert "max-cll=1000,400" in payload
    assert "hdr10-opt=1" in payload


def test_hdr_args_x265_hlg_omits_hdr10_opt():
    args = hdr_codec_args("libx265", _pq_info(transfer="hlg"))
    payload = args[args.index("-x265-params") + 1]
    assert "transfer=arib-std-b67" in payload
    assert "hdr10-opt=1" not in payload


def test_hdr_args_svtav1_uses_av1_enums():
    args = hdr_codec_args("libsvtav1", _pq_info())
    assert "-svtav1-params" in args
    payload = args[args.index("-svtav1-params") + 1]
    assert "color-primaries=9" in payload  # BT.2020
    assert "transfer-characteristics=16" in payload  # PQ
    assert "matrix-coefficients=9" in payload


def test_hdr_args_svtav1_hlg_uses_transfer_18():
    args = hdr_codec_args("libsvtav1", _pq_info(transfer="hlg"))
    payload = args[args.index("-svtav1-params") + 1]
    assert "transfer-characteristics=18" in payload  # HLG


def test_hdr_args_nvenc_hevc_uses_p010_main10():
    args = hdr_codec_args("hevc_nvenc", _pq_info())
    assert "-pix_fmt" in args
    assert args[args.index("-pix_fmt") + 1] == "p010le"
    assert "-profile:v" in args
    assert args[args.index("-profile:v") + 1] == "main10"


@pytest.mark.parametrize(
    "encoder",
    ("hevc_qsv", "hevc_amf", "hevc_videotoolbox"),
)
def test_hdr_args_hevc_hardware_family_forces_main10(encoder: str):
    args = hdr_codec_args(encoder, _pq_info())
    assert "-pix_fmt" in args
    assert args[args.index("-pix_fmt") + 1] == "p010le"
    assert "-profile:v" in args
    assert args[args.index("-profile:v") + 1] == "main10"
    assert "-color_trc" in args
    assert args[args.index("-color_trc") + 1] == "smpte2084"


@pytest.mark.parametrize(
    "encoder",
    ("av1_nvenc", "av1_qsv", "av1_amf"),
)
def test_hdr_args_av1_hardware_family_forces_p010_without_hevc_profile(encoder: str):
    args = hdr_codec_args(encoder, _pq_info())
    assert "-pix_fmt" in args
    assert args[args.index("-pix_fmt") + 1] == "p010le"
    assert "-profile:v" not in args
    assert "-color_primaries" in args
    assert args[args.index("-color_trc") + 1] == "smpte2084"


def test_hdr_args_libaom_av1_emits_global_color_tags_only():
    args = hdr_codec_args("libaom-av1", _pq_info(transfer="hlg"))
    assert "-color_primaries" in args
    assert args[args.index("-color_trc") + 1] == "arib-std-b67"
    assert "-pix_fmt" not in args
    assert "-svtav1-params" not in args


def test_hdr_args_unknown_encoder_returns_empty():
    args = hdr_codec_args("libthisdoesnotexist", _pq_info())
    assert args == ()


# ---------------------------------------------------------------------------
# Model resolution
# ---------------------------------------------------------------------------


def test_select_hdr_vmaf_model_returns_none_when_absent(tmp_path: Path):
    # Empty model dir.
    assert select_hdr_vmaf_model(tmp_path) is None


def test_select_hdr_vmaf_model_finds_shipped_json(tmp_path: Path):
    (tmp_path / "vmaf_v0.6.1.json").write_text("{}")  # SDR — must be ignored
    target = tmp_path / "vmaf_hdr_v0.6.1.json"
    target.write_text("{}")
    found = select_hdr_vmaf_model(tmp_path)
    assert found == target


def test_select_hdr_vmaf_model_picks_latest_when_multiple(tmp_path: Path):
    (tmp_path / "vmaf_hdr_v0.6.0.json").write_text("{}")
    latest = tmp_path / "vmaf_hdr_v0.6.1.json"
    latest.write_text("{}")
    found = select_hdr_vmaf_model(tmp_path)
    assert found == latest


def test_select_hdr_vmaf_model_handles_missing_dir(tmp_path: Path):
    nope = tmp_path / "does_not_exist"
    assert select_hdr_vmaf_model(nope) is None


# ---------------------------------------------------------------------------
# HDR model port — registration + transfer-aware resolution
# ---------------------------------------------------------------------------


def test_hdr_model_name_for_pq_returns_canonical_filename():
    assert hdr_model_name_for("pq") == HDR_MODEL_FILENAME
    assert hdr_model_name_for("PQ") == HDR_MODEL_FILENAME  # case-insensitive


def test_hdr_model_name_for_hlg_returns_canonical_filename():
    assert hdr_model_name_for("hlg") == HDR_MODEL_FILENAME


def test_hdr_model_name_for_sdr_returns_none():
    assert hdr_model_name_for(None) is None
    assert hdr_model_name_for("") is None
    assert hdr_model_name_for("bt709") is None


def test_select_hdr_vmaf_model_prefers_canonical_for_pq(tmp_path: Path):
    canonical = tmp_path / HDR_MODEL_FILENAME
    canonical.write_text("{}")
    # An older variant must NOT be picked when the canonical is present.
    (tmp_path / "vmaf_hdr_v0.5.0.json").write_text("{}")
    found = select_hdr_vmaf_model(tmp_path, transfer="pq")
    assert found == canonical


def test_select_hdr_vmaf_model_falls_back_to_glob_when_canonical_absent(tmp_path: Path):
    older = tmp_path / "vmaf_hdr_v0.6.0.json"
    older.write_text("{}")
    # No canonical file -- glob picks the only HDR JSON available.
    found = select_hdr_vmaf_model(tmp_path, transfer="hlg")
    assert found == older


def test_select_hdr_vmaf_model_returns_none_for_sdr_transfer_in_empty_dir(tmp_path: Path):
    # Empty directory + SDR transfer -> None (no HDR routing required).
    reset_hdr_model_warning()
    assert select_hdr_vmaf_model(tmp_path, transfer=None) is None


def test_select_hdr_vmaf_model_logs_warning_once_when_missing(
    tmp_path: Path,
    caplog: pytest.LogCaptureFixture,
):
    """Single-shot warning so corpus runs over many SDR sources stay quiet."""
    reset_hdr_model_warning()
    with caplog.at_level("WARNING", logger="vmaftune.hdr"):
        # Two consecutive misses -> only one warning recorded.
        select_hdr_vmaf_model(tmp_path, transfer="pq")
        select_hdr_vmaf_model(tmp_path, transfer="pq")
    warns = [r for r in caplog.records if "HDR VMAF model unavailable" in r.message]
    assert len(warns) == 1
    assert HDR_MODEL_FILENAME in warns[0].message


def test_select_hdr_vmaf_model_does_not_warn_when_canonical_present(
    tmp_path: Path,
    caplog: pytest.LogCaptureFixture,
):
    (tmp_path / HDR_MODEL_FILENAME).write_text("{}")
    reset_hdr_model_warning()
    with caplog.at_level("WARNING", logger="vmaftune.hdr"):
        select_hdr_vmaf_model(tmp_path, transfer="pq")
    warns = [r for r in caplog.records if "HDR VMAF model unavailable" in r.message]
    assert warns == []


def test_select_hdr_vmaf_model_legacy_call_without_transfer_still_works(tmp_path: Path):
    """ADR-0300 callers used the no-arg signature — keep it green."""
    target = tmp_path / "vmaf_hdr_v0.6.1.json"
    target.write_text("{}")
    assert select_hdr_vmaf_model(tmp_path) == target


# ---------------------------------------------------------------------------
# Corpus integration — HDR row population
# ---------------------------------------------------------------------------

# These two tests exercise the full ``iter_rows`` HDR-detection +
# codec-arg injection + row-field emission path. The CLI surface
# (``--auto-hdr`` / ``--force-sdr`` / ``--force-hdr-pq`` /
# ``--force-hdr-hlg``) shipped in PR #434 / ADR-0261; the actual
# hookup into the encode loop landed in the follow-up that wires
# ``detect_hdr`` + ``hdr_codec_args`` + ``select_hdr_vmaf_model``
# into ``iter_rows`` (HP-2, ADR-0300 status update 2026-05-08).


def test_corpus_emits_hdr_fields_when_source_is_hdr(tmp_path: Path):
    """End-to-end: HDR PQ source through corpus.iter_rows populates v2 fields."""
    from vmaftune.corpus import CorpusJob, CorpusOptions, iter_rows

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 1024)

    captured_cmds: list[list[str]] = []

    def fake_encode_run(cmd, capture_output, text, check):
        captured_cmds.append(list(cmd))
        Path(cmd[-1]).write_bytes(b"\x00" * 4096)
        return _FakeCompleted(
            returncode=0,
            stderr="ffmpeg version 6.1.1\nx264 - core 164 r3107\n",
        )

    def fake_score_run(cmd, capture_output, text, check):
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 90.0}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p10le",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        keep_encodes=False,
        src_sha256=False,
        hdr_mode="force-hdr-pq",  # bypass ffprobe
    )

    rows = list(iter_rows(job, opts, encode_runner=fake_encode_run, score_runner=fake_score_run))
    assert len(rows) == 1
    row = rows[0]
    assert row["hdr_transfer"] == "pq"
    assert row["hdr_primaries"] == "bt2020"
    assert row["hdr_forced"] is True
    # libx264 emits the global -color_* family on the encode cmd.
    assert "-color_trc" in captured_cmds[0]
    assert captured_cmds[0][captured_cmds[0].index("-color_trc") + 1] == "smpte2084"


def test_corpus_force_sdr_skips_hdr_path(tmp_path: Path):
    """force-sdr mode emits empty HDR fields and no -color_* flags."""
    from vmaftune.corpus import CorpusJob, CorpusOptions, iter_rows

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 1024)
    captured_cmds: list[list[str]] = []

    def fake_encode_run(cmd, capture_output, text, check):
        captured_cmds.append(list(cmd))
        Path(cmd[-1]).write_bytes(b"\x00" * 4096)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\nx264 - core 164\n")

    def fake_score_run(cmd, capture_output, text, check):
        out_idx = cmd.index("--output") + 1
        Path(cmd[out_idx]).parent.mkdir(parents=True, exist_ok=True)
        Path(cmd[out_idx]).write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 91.0}}}))
        return _FakeCompleted(returncode=0, stderr="")

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
        hdr_mode="force-sdr",
    )

    rows = list(iter_rows(job, opts, encode_runner=fake_encode_run, score_runner=fake_score_run))
    assert rows[0]["hdr_transfer"] == ""
    assert rows[0]["hdr_forced"] is True
    assert "-color_trc" not in captured_cmds[0]


def test_master_display_format_shape():
    """Sanity-check the x265 master-display string layout."""
    info = _pq_info()
    args = hdr_codec_args("libx265", info)
    payload = args[args.index("-x265-params") + 1]
    assert "master-display=G(" in payload
    assert "B(" in payload and "R(" in payload
    assert "WP(" in payload and "L(" in payload
