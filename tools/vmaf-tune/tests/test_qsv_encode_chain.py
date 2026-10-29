# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""QSV device chain and hardware probes on the real encode path (ADR-0601).

The VA-API / QSV device chain and the ``hwupload`` filter used to be
built only by ``compare``'s availability probe; every real QSV encode
(``corpus``, ``ladder``, the bisect behind ``compare``) went out without
them and failed. The chain now comes from the adapter, and
``build_ffmpeg_command`` and the probe both use it. No QSV encode runs
here: the argv and the probe wiring are tested with fakes.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import cli, compare, hdr, hw_devices
from vmaftune.codec_adapters._qsv_common import QSV_UPLOAD_FILTER
from vmaftune.encode import EncodeRequest, build_ffmpeg_command

QSV = ("h264_qsv", "hevc_qsv", "av1_qsv")
DEVICE = "/dev/dri/renderD131"


def _request(tmp_path: Path, encoder: str, **kw) -> EncodeRequest:
    base = {
        "source": tmp_path / "ref.yuv",
        "width": 1920,
        "height": 1080,
        "pix_fmt": "yuv420p",
        "framerate": 24.0,
        "encoder": encoder,
        "preset": "medium",
        "crf": 23,
        "output": tmp_path / "out.mp4",
    }
    base.update(kw)
    return EncodeRequest(**base)


def _pre_input(argv: list[str]) -> list[str]:
    return argv[: argv.index("-i")]


@pytest.mark.parametrize("encoder", QSV)
def test_qsv_encode_carries_device_chain_before_input(tmp_path, encoder):
    argv = build_ffmpeg_command(_request(tmp_path, encoder, vaapi_device=DEVICE))
    head = _pre_input(argv)
    assert head[head.index("-init_hw_device") + 1] == f"vaapi=va:{DEVICE}"
    assert "qsv=qsv_dev@va" in head
    # The upload goes to the QSV device: with `-filter_hw_device va` hwupload
    # produced vaapi frames and the filter graph failed before the encoder.
    assert head[head.index("-filter_hw_device") + 1] == "qsv_dev"
    tail = argv[argv.index("-i") :]
    assert tail.count("-vf") == 1
    assert tail[tail.index("-vf") + 1] == QSV_UPLOAD_FILTER
    assert tail.index("-vf") < len(tail) - 1  # before the output path


def test_qsv_upload_joins_the_rung_scale_chain(tmp_path):
    """A second -vf would replace the first: ffmpeg keeps the last one."""
    argv = build_ffmpeg_command(
        _request(tmp_path, "h264_qsv", extra_params=("-vf", "scale=640:360"), vaapi_device=DEVICE)
    )
    assert argv.count("-vf") == 1
    assert argv[argv.index("-vf") + 1] == f"scale=640:360,{QSV_UPLOAD_FILTER}"


@pytest.mark.parametrize("encoder", QSV)
@pytest.mark.parametrize(
    ("pix_fmt", "extra", "surface"),
    [
        ("yuv420p", (), "nv12"),
        ("nv12", ("-profile:v", "main"), "nv12"),
        ("p010le", (), "p010le"),
        ("yuv420p10le", (), "p010le"),
        ("yuv420p", ("-pix_fmt", "yuv420p10le"), "p010le"),
        ("yuv420p", ("-pix_fmt", "p010le"), "p010le"),
        ("yuv420p", ("-profile:v", "main10"), "p010le"),
        ("yuv420p10le", ("-pix_fmt", "yuv420p"), "nv12"),
        ("yuv420p", ("-metadata", "main10"), "nv12"),
        ("yuv420p", ("-pix_fmt",), "nv12"),
    ],
)
def test_qsv_upload_follows_output_depth(tmp_path, encoder, pix_fmt, extra, surface):
    argv = build_ffmpeg_command(
        _request(
            tmp_path,
            encoder,
            pix_fmt=pix_fmt,
            extra_params=("-vf", "scale=640:360", *extra),
        )
    )
    assert argv.count("-vf") == 1
    assert argv[argv.index("-vf") + 1] == (
        f"scale=640:360,format={surface},hwupload=extra_hw_frames=64"
    )


@pytest.mark.parametrize("encoder", ["hevc_qsv", "av1_qsv"])
def test_qsv_hdr_upload_uses_p010le(tmp_path, encoder):
    info = hdr.HdrInfo("pq", "bt2020", "bt2020nc", "tv", "yuv420p10le")
    extra = hdr.hdr_codec_args(encoder, info)
    argv = build_ffmpeg_command(_request(tmp_path, encoder, extra_params=extra))
    assert argv[argv.index("-vf") + 1] == "format=p010le,hwupload=extra_hw_frames=64"


@pytest.mark.parametrize("encoder", ["libx264", "h264_nvenc", "h264_amf", "libsvtav1"])
def test_non_qsv_encodes_get_no_device_chain(tmp_path, encoder):
    argv = build_ffmpeg_command(_request(tmp_path, encoder, extra_params=("-vf", "scale=1:1")))
    assert "-init_hw_device" not in argv
    assert "-filter_hw_device" not in argv
    assert argv[argv.index("-vf") + 1] == "scale=1:1"


def test_probe_and_encode_share_one_chain(tmp_path):
    """compare's dummy encode and a real encode initialise the device identically."""
    probe = compare._hw_probe_argv("ffmpeg", "hevc_qsv", DEVICE)
    encode = build_ffmpeg_command(_request(tmp_path, "hevc_qsv", vaapi_device=DEVICE))
    assert _device_chain(probe) == _device_chain(encode)
    assert len(_device_chain(probe)) == 6
    assert probe[probe.index("-vf") + 1] == encode[encode.index("-vf") + 1]


def _device_chain(argv: list[str]) -> list[str]:
    """The argv from the first -init_hw_device through the -filter_hw_device value."""
    start = argv.index("-init_hw_device")
    return argv[start : argv.index("-filter_hw_device") + 2]


def test_device_resolution_order(monkeypatch):
    """Explicit path, then --vaapi-device of the command, then the env var."""
    monkeypatch.setenv(hw_devices.VAAPI_DEVICE_ENV, "/dev/dri/renderD140")
    assert hw_devices.resolve_vaapi_device("auto") == "/dev/dri/renderD140"
    with hw_devices.session_vaapi_device("/dev/dri/renderD141"):
        assert hw_devices.resolve_vaapi_device("auto") == "/dev/dri/renderD141"
        assert hw_devices.resolve_vaapi_device("/dev/dri/renderD142") == "/dev/dri/renderD142"
    # The session ends with the block; `auto` / None leave resolution alone.
    assert hw_devices.resolve_vaapi_device("auto") == "/dev/dri/renderD140"
    with hw_devices.session_vaapi_device("auto"):
        assert hw_devices.resolve_vaapi_device(None) == "/dev/dri/renderD140"


def test_env_device_reaches_every_qsv_encode(tmp_path, monkeypatch):
    monkeypatch.setenv(hw_devices.VAAPI_DEVICE_ENV, DEVICE)
    argv = build_ffmpeg_command(_request(tmp_path, "av1_qsv"))
    assert f"vaapi=va:{DEVICE}" in argv


def test_compare_vaapi_device_flag_holds_for_the_encodes(tmp_path, monkeypatch):
    """`compare --vaapi-device` used to reach the probe only."""
    seen: list[list[str]] = []

    def fake_session(args):
        seen.append(build_ffmpeg_command(_request(tmp_path, "h264_qsv")))
        return 0

    monkeypatch.delenv(hw_devices.VAAPI_DEVICE_ENV, raising=False)
    monkeypatch.setattr(cli, "_run_compare_in_session", fake_session)
    args = cli._build_parser().parse_args(
        ["compare", "--src", str(tmp_path / "ref.yuv"), "--vaapi-device", DEVICE]
    )
    assert cli._run_compare(args) == 0
    assert f"vaapi=va:{DEVICE}" in seen[0]


def _corpus_args(tmp_path: Path, encoder: str):
    return cli._build_parser().parse_args(
        [
            "corpus",
            "--source",
            str(tmp_path / "ref.yuv"),
            "--width",
            "64",
            "--height",
            "64",
            "--encoder",
            encoder,
            "--preset",
            "medium",
            "--crf",
            "23",
            "--output",
            str(tmp_path / "c.jsonl"),
            "--score-backend",
            "cpu",
        ]
    )


def test_corpus_refuses_an_unavailable_hardware_encoder(tmp_path, monkeypatch, capsys):
    calls: list[str] = []

    def fake_probe(encoder, *, ffmpeg_bin="ffmpeg", **_kw):
        calls.append(encoder)
        return False, f"hardware encoder not available: {encoder} dummy encode failed"

    monkeypatch.setattr(cli, "select_backend", lambda prefer, vmaf_bin: "cpu")
    monkeypatch.setattr(compare, "probe_encoder_available", fake_probe)
    assert cli._run_corpus(_corpus_args(tmp_path, "hevc_qsv")) == 2
    assert calls == ["hevc_qsv"]
    assert "hevc_qsv dummy encode failed" in capsys.readouterr().err


def test_corpus_does_not_probe_software_encoders(monkeypatch):
    def boom(*_a, **_kw):
        raise AssertionError("software encoders are not probed")

    monkeypatch.setattr(compare, "probe_encoder_available", boom)
    cli._require_hardware_encoder("libx264", "ffmpeg")


@pytest.mark.parametrize(
    "encoder",
    ["h264_videotoolbox", "hevc_videotoolbox", "av1_videotoolbox", "prores_videotoolbox"],
)
def test_videotoolbox_gets_the_dummy_encode_probe(encoder):
    """VideoToolbox had no host probe: a listed encoder passed on any host."""
    calls: list[list[str]] = []

    class _Done:
        def __init__(self, rc, out):
            self.returncode, self.stdout = rc, out

    def run(argv, timeout=30.0):
        calls.append(list(argv))
        if "-encoders" in argv:
            return _Done(0, f" V..... {encoder}  VideoToolbox\n".encode())
        return _Done(1, b"Error: no VideoToolbox device")

    ok, reason = compare.probe_encoder_available(encoder, runner=run)
    assert ok is False and "dummy encode failed" in reason
    assert len(calls) == 2
