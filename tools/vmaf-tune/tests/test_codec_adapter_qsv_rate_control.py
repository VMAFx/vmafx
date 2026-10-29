# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""ICQ probe, explicit fallback, process cache and override contract."""

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import SimpleNamespace

import pytest

from vmaftune import compare, corpus
from vmaftune.codec_adapters import _qsv_common as qsv
from vmaftune.codec_adapters import get_adapter
from vmaftune.encode import EncodeRequest, run_encode


@pytest.fixture(autouse=True)
def isolated_probe(monkeypatch):
    monkeypatch.setenv("VMAFTUNE_QSV_RATE_CONTROL", "auto")
    qsv._QSV_RC_CACHE.clear()
    yield
    qsv._QSV_RC_CACHE.clear()


@pytest.mark.parametrize("encoder", ["h264_qsv", "hevc_qsv", "av1_qsv"])
@pytest.mark.parametrize(
    "rc,mode,knob", [(0, "icq", "-global_quality"), (183, "cqp-fallback", "-q:v")]
)
@pytest.mark.parametrize("quality", [1, 23, 51])
def test_encode_probe_and_named_output(tmp_path, encoder, rc, mode, knob, quality):
    calls = []

    def runner(argv, **kwargs):
        calls.append(argv)
        return SimpleNamespace(
            returncode=rc if "lavfi" in argv else 0, stderr="Invalid FrameType:0", stdout=""
        )

    req = EncodeRequest(
        tmp_path / "in.yuv",
        320,
        240,
        "yuv420p",
        24,
        encoder,
        "medium",
        quality,
        tmp_path / "out.mkv",
    )
    result = run_encode(req, runner=runner)
    assert result.qsv_rate_control == mode
    assert calls[0][calls[0].index("-frames:v") + 1] == "8"
    assert calls[1][calls[1].index(knob) + 1] == str(quality)
    assert ("-global_quality" in calls[1]) == (mode == "icq")
    assert bool(result.qsv_rate_control_reason) == (rc != 0)
    if rc:
        assert "Invalid FrameType:0" in result.qsv_rate_control_reason
    report_row = (
        compare._PredicateRun(lambda *args: None, Path("in"), None)
        .settle(encoder, lambda: compare.RecommendResult(encoder, quality, 1, 1, 95))
        .to_row(95)
    )
    assert report_row["qsv_rate_control"] == mode
    assert report_row["provenance"]["qsv_rate_control"] == mode
    _assert_corpus_row_names_mode(req, encoder, quality, result, mode)


def _assert_corpus_row_names_mode(req, encoder, quality, result, mode):
    row = corpus._row_for(
        job=corpus.CorpusJob(
            source=req.source,
            width=320,
            height=240,
            pix_fmt="yuv420p",
            framerate=24,
            duration_s=1,
            cells=(("medium", quality),),
        ),
        opts=corpus.CorpusOptions(encoder=encoder),
        preset="medium",
        crf=quality,
        src_sha="",
        enc_res=result,
        score_res=SimpleNamespace(
            vmaf_score=95,
            score_time_ms=1,
            vmaf_binary_version="test",
            exit_status=0,
            feature_means={},
            feature_stds={},
        ),
    )
    assert row["qsv_rate_control"] == mode
    assert row["provenance"]["qsv_rate_control"] == mode


def test_concurrent_cache_and_device_encoder_runtime_keys():
    calls = []

    def runner(argv):
        calls.append(argv)
        return SimpleNamespace(returncode=0)

    def probe(_):
        return qsv.qsv_rate_control("h264_qsv", vaapi_device="/dev/dri/renderD129", runner=runner)

    with ThreadPoolExecutor(max_workers=4) as pool:
        assert all(r.mode == "icq" for r in pool.map(probe, range(16)))
    assert len(calls) == 1
    qsv.qsv_rate_control("hevc_qsv", vaapi_device="/dev/dri/renderD129", runner=runner)
    qsv.qsv_rate_control("h264_qsv", vaapi_device="/dev/dri/renderD130", runner=runner)
    qsv.qsv_rate_control(
        "h264_qsv", vaapi_device="/dev/dri/renderD129", ffmpeg_bin="other", runner=runner
    )
    assert len(calls) == 4


@pytest.mark.parametrize("override,mode", [("icq", "icq"), ("cqp", "cqp-fallback")])
def test_override_skips_probe(monkeypatch, override, mode):
    monkeypatch.setenv("VMAFTUNE_QSV_RATE_CONTROL", override)

    def forbidden(argv):
        pytest.fail("override must skip probe")

    assert qsv.qsv_rate_control("h264_qsv", runner=forbidden).mode == mode
    assert qsv.cached_qsv_rate_control("h264_qsv").mode == mode


def test_invalid_override(monkeypatch):
    monkeypatch.setenv("VMAFTUNE_QSV_RATE_CONTROL", "typo")
    with pytest.raises(ValueError, match="VMAFTUNE_QSV_RATE_CONTROL"):
        qsv.qsv_rate_control("h264_qsv")


@pytest.mark.parametrize("quality", [0, 52])
def test_quality_range_same_in_both_modes(quality):
    for mode in ("icq", "cqp-fallback"):
        with pytest.raises(ValueError):
            get_adapter("h264_qsv").ffmpeg_codec_args("medium", quality, rate_control=mode)


def test_timeout_is_named_fallback():
    import subprocess

    def runner(argv):
        raise subprocess.TimeoutExpired(argv, 30)

    result = qsv.qsv_rate_control("h264_qsv", runner=runner)
    assert result.mode == "cqp-fallback"
    assert "timed out" in result.reason


def test_override_never_marks_software_rows(monkeypatch):
    monkeypatch.setenv("VMAFTUNE_QSV_RATE_CONTROL", "cqp")
    row = compare._apply_row_metadata(
        "libx264", compare.RecommendResult("libx264", 23, 1, 1, 95), None
    ).to_row(95)
    assert "qsv_rate_control" not in row


def test_failed_compare_row_keeps_fallback_provenance():
    qsv.qsv_rate_control("h264_qsv", runner=lambda argv: SimpleNamespace(returncode=183))

    def failed():
        raise RuntimeError("encode failed")

    result = compare._PredicateRun(lambda *args: None, Path("in"), None).settle("h264_qsv", failed)
    assert not result.ok
    assert result.to_row(95)["qsv_rate_control"] == "cqp-fallback"


def test_probe_cache_isolated_from_override(monkeypatch):
    calls = []

    def runner(argv):
        calls.append(argv)
        return SimpleNamespace(returncode=183)

    assert qsv.qsv_rate_control("h264_qsv", runner=runner).mode == "cqp-fallback"
    monkeypatch.setenv("VMAFTUNE_QSV_RATE_CONTROL", "icq")
    assert qsv.qsv_rate_control("h264_qsv", runner=runner).mode == "icq"
    monkeypatch.setenv("VMAFTUNE_QSV_RATE_CONTROL", "auto")
    assert qsv.qsv_rate_control("h264_qsv", runner=runner).mode == "cqp-fallback"
    assert len(calls) == 1


def test_encode_cache_keys_distinguish_mode_and_device(monkeypatch):
    sweep = SimpleNamespace(
        job=corpus.CorpusJob(Path("ref.yuv"), 320, 240, "yuv420p", 24, 1, (("medium", 23),)),
        opts=corpus.CorpusOptions(encoder="h264_qsv"),
        adapter=get_adapter("h264_qsv"),
        hdr_extra_params=(),
        score_model="vmaf_v0.6.1",
        ffmpeg_version="test",
        clip_seconds=0,
        start_s=0,
    )
    monkeypatch.setenv("VMAFTUNE_QSV_RATE_CONTROL", "icq")
    monkeypatch.setenv("VMAFTUNE_VAAPI_DEVICE", "/dev/dri/renderD129")
    first = corpus._cell_params(sweep, "medium", 23)
    monkeypatch.setenv("VMAFTUNE_QSV_RATE_CONTROL", "cqp")
    second = corpus._cell_params(sweep, "medium", 23)
    assert first["settings"]["qsv_rate_control"] != second["settings"]["qsv_rate_control"]
    monkeypatch.setenv("VMAFTUNE_VAAPI_DEVICE", "/dev/dri/renderD130")
    third = corpus._cell_params(sweep, "medium", 23)
    assert second["settings"]["qsv_device"] != third["settings"]["qsv_device"]
