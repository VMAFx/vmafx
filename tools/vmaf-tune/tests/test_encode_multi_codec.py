# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Multi-codec dispatcher tests for ``encode.py`` (ADR-0297).

Covers:

- Adapter-driven argv composition (``ffmpeg_codec_args``).
- Fallback to the legacy x264-CRF shape when an adapter is missing or
  doesn't expose the contract method.
- ``parse_versions(stderr, encoder=...)`` per-codec probe selection.
- Smoke-mocked end-to-end ``run_encode`` against fake adapters
  representing the in-flight per-codec PRs (libx265, libsvtav1,
  libaom-av1, libvpx-vp9, libvvenc, h264_nvenc, hevc_qsv,
  h264_videotoolbox, h264_amf).
"""

from __future__ import annotations

import dataclasses
import sys
from pathlib import Path
from unittest import mock

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import codec_adapters as ca
from vmaftune.encode import (
    EncodeRequest,
    build_ffmpeg_command,
    parse_versions,
    run_encode,
)

# ---------------------------------------------------------------------------
# Fake adapters — model the contract each in-flight PR ships.
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class _X265Adapter:
    name: str = "libx265"
    encoder: str = "libx265"
    quality_knob: str = "crf"
    quality_range: tuple[int, int] = (15, 40)
    quality_default: int = 28
    invert_quality: bool = True
    presets: tuple[str, ...] = ("medium", "slow", "veryslow")

    def validate(self, preset: str, q: int) -> None:
        if preset not in self.presets:
            raise ValueError(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        return ["-c:v", self.encoder, "-preset", preset, "-crf", str(quality)]

    def extra_params(self) -> tuple[str, ...]:
        return ()


@dataclasses.dataclass(frozen=True)
class _SvtAv1Adapter:
    name: str = "libsvtav1"
    encoder: str = "libsvtav1"
    quality_knob: str = "crf"
    quality_range: tuple[int, int] = (20, 50)
    quality_default: int = 35
    invert_quality: bool = True
    presets: tuple[str, ...] = ("4", "6", "8", "10")

    def validate(self, preset: str, q: int) -> None:
        if preset not in self.presets:
            raise ValueError(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        # SVT-AV1: -preset is an integer 0..13, -crf is the quality knob.
        return ["-c:v", self.encoder, "-preset", preset, "-crf", str(quality)]

    def extra_params(self) -> tuple[str, ...]:
        return ("-svtav1-params", "tune=0")


@dataclasses.dataclass(frozen=True)
class _LibAomAdapter:
    name: str = "libaom-av1"
    encoder: str = "libaom-av1"
    quality_knob: str = "crf"
    quality_range: tuple[int, int] = (20, 50)
    quality_default: int = 32
    invert_quality: bool = True
    presets: tuple[str, ...] = ("good-cpu0", "good-cpu4", "good-cpu8")

    def validate(self, preset: str, q: int) -> None:
        if preset not in self.presets:
            raise ValueError(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        # libaom-av1: -crf is the quality knob, -b:v 0 forces CRF mode,
        # -cpu-used picks the speed preset.
        cpu = preset.removeprefix("good-cpu")
        return [
            "-c:v",
            self.encoder,
            "-cpu-used",
            cpu,
            "-crf",
            str(quality),
            "-b:v",
            "0",
        ]

    def extra_params(self) -> tuple[str, ...]:
        return ("-row-mt", "1")


@dataclasses.dataclass(frozen=True)
class _LibVpxAdapter:
    name: str = "libvpx-vp9"
    encoder: str = "libvpx-vp9"
    quality_knob: str = "crf"
    quality_range: tuple[int, int] = (20, 50)
    quality_default: int = 32
    invert_quality: bool = True
    presets: tuple[str, ...] = ("good-cpu0", "good-cpu4")

    def validate(self, preset: str, q: int) -> None:
        if preset not in self.presets:
            raise ValueError(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        cpu = preset.removeprefix("good-cpu")
        return [
            "-c:v",
            self.encoder,
            "-deadline",
            "good",
            "-cpu-used",
            cpu,
            "-crf",
            str(quality),
            "-b:v",
            "0",
        ]


@dataclasses.dataclass(frozen=True)
class _VvencAdapter:
    name: str = "libvvenc"
    encoder: str = "libvvenc"
    quality_knob: str = "qp"
    quality_range: tuple[int, int] = (20, 45)
    quality_default: int = 30
    invert_quality: bool = True
    presets: tuple[str, ...] = ("faster", "fast", "medium", "slow")

    def validate(self, preset: str, q: int) -> None:
        if preset not in self.presets:
            raise ValueError(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        # VVenC: -preset + -qp for constant-QP mode.
        return ["-c:v", self.encoder, "-preset", preset, "-qp", str(quality)]


@dataclasses.dataclass(frozen=True)
class _NvencAdapter:
    name: str = "h264_nvenc"
    encoder: str = "h264_nvenc"
    quality_knob: str = "cq"
    quality_range: tuple[int, int] = (15, 40)
    quality_default: int = 23
    invert_quality: bool = True
    presets: tuple[str, ...] = ("p1", "p4", "p7")

    def validate(self, preset: str, q: int) -> None:
        if preset not in self.presets:
            raise ValueError(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        # NVENC: -preset + -rc vbr -cq for VBR-with-quality-target mode.
        return [
            "-c:v",
            self.encoder,
            "-preset",
            preset,
            "-rc",
            "vbr",
            "-cq",
            str(quality),
        ]


@dataclasses.dataclass(frozen=True)
class _QsvAdapter:
    name: str = "hevc_qsv"
    encoder: str = "hevc_qsv"
    quality_knob: str = "global_quality"
    quality_range: tuple[int, int] = (15, 40)
    quality_default: int = 23
    invert_quality: bool = True
    presets: tuple[str, ...] = ("veryfast", "medium", "veryslow")

    def validate(self, preset: str, q: int) -> None:
        if preset not in self.presets:
            raise ValueError(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        return [
            "-c:v",
            self.encoder,
            "-preset",
            preset,
            "-global_quality",
            str(quality),
        ]


@dataclasses.dataclass(frozen=True)
class _AmfAdapter:
    name: str = "h264_amf"
    encoder: str = "h264_amf"
    quality_knob: str = "qp_i"
    quality_range: tuple[int, int] = (15, 40)
    quality_default: int = 23
    invert_quality: bool = True
    presets: tuple[str, ...] = ("speed", "balanced", "quality")

    def validate(self, preset: str, q: int) -> None:
        if preset not in self.presets:
            raise ValueError(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        return [
            "-c:v",
            self.encoder,
            "-quality",
            preset,
            "-rc",
            "cqp",
            "-qp_i",
            str(quality),
            "-qp_p",
            str(quality),
        ]


@dataclasses.dataclass(frozen=True)
class _VideoToolboxAdapter:
    name: str = "h264_videotoolbox"
    encoder: str = "h264_videotoolbox"
    quality_knob: str = "q:v"
    quality_range: tuple[int, int] = (1, 100)
    quality_default: int = 65
    invert_quality: bool = False
    presets: tuple[str, ...] = ("default",)

    def validate(self, preset: str, q: int) -> None:
        if preset not in self.presets:
            raise ValueError(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        return ["-c:v", self.encoder, "-q:v", str(quality)]


@dataclasses.dataclass(frozen=True)
class _LegacyNoArgsAdapter:
    """Adapter that DELIBERATELY does not implement ``ffmpeg_codec_args``.

    Models a not-yet-migrated adapter — the dispatcher must fall back
    to the legacy x264-CRF shape and not crash.
    """

    name: str = "legacy_codec"
    encoder: str = "libx264"  # the fallback assumes x264 shape
    quality_knob: str = "crf"
    quality_range: tuple[int, int] = (15, 40)
    quality_default: int = 23
    invert_quality: bool = True
    presets: tuple[str, ...] = ("medium",)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _req(encoder: str, preset: str, quality: int, output: str = "out.mp4") -> EncodeRequest:
    return EncodeRequest(
        source=Path("ref.yuv"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder=encoder,
        preset=preset,
        crf=quality,
        output=Path(output),
    )


def _patch_registry(*adapters: object):
    """Replace ``_REGISTRY`` and re-route ``get_adapter`` for the test."""
    fake = {a.encoder: a for a in adapters}  # type: ignore[attr-defined]

    def _get(name: str) -> object:
        if name not in fake:
            raise KeyError(name)
        return fake[name]

    return mock.patch.object(ca, "_REGISTRY", fake), mock.patch.object(ca, "get_adapter", _get)


# ---------------------------------------------------------------------------
# argv composition — one test per registered codec.
# ---------------------------------------------------------------------------


def test_dispatcher_x264_argv_unchanged():
    # x264 is already registered on master; no patching needed.
    cmd = build_ffmpeg_command(_req("libx264", "medium", 23))
    assert "-c:v" in cmd and cmd[cmd.index("-c:v") + 1] == "libx264"
    assert cmd[cmd.index("-preset") + 1] == "medium"
    assert cmd[cmd.index("-crf") + 1] == "23"
    assert cmd[-1] == "out.mp4"


@pytest.mark.parametrize(
    "adapter,preset,quality,must_have",
    [
        (_X265Adapter(), "slow", 28, [("-c:v", "libx265"), ("-preset", "slow"), ("-crf", "28")]),
        (
            _SvtAv1Adapter(),
            "8",
            35,
            [
                ("-c:v", "libsvtav1"),
                ("-preset", "8"),
                ("-crf", "35"),
                ("-svtav1-params", "tune=0"),
            ],
        ),
        (
            _LibAomAdapter(),
            "good-cpu4",
            32,
            [
                ("-c:v", "libaom-av1"),
                ("-cpu-used", "4"),
                ("-crf", "32"),
                ("-b:v", "0"),
                ("-row-mt", "1"),
            ],
        ),
        (
            _LibVpxAdapter(),
            "good-cpu0",
            32,
            [
                ("-c:v", "libvpx-vp9"),
                ("-deadline", "good"),
                ("-cpu-used", "0"),
                ("-crf", "32"),
                ("-b:v", "0"),
            ],
        ),
        (
            _VvencAdapter(),
            "medium",
            30,
            [("-c:v", "libvvenc"), ("-preset", "medium"), ("-qp", "30")],
        ),
        (
            _NvencAdapter(),
            "p4",
            23,
            [
                ("-c:v", "h264_nvenc"),
                ("-preset", "p4"),
                ("-rc", "vbr"),
                ("-cq", "23"),
            ],
        ),
        (
            _QsvAdapter(),
            "medium",
            25,
            [("-c:v", "hevc_qsv"), ("-preset", "medium"), ("-global_quality", "25")],
        ),
        (
            _AmfAdapter(),
            "balanced",
            23,
            [
                ("-c:v", "h264_amf"),
                ("-quality", "balanced"),
                ("-rc", "cqp"),
                ("-qp_i", "23"),
                ("-qp_p", "23"),
            ],
        ),
        (
            _VideoToolboxAdapter(),
            "default",
            65,
            [("-c:v", "h264_videotoolbox"), ("-q:v", "65")],
        ),
    ],
)
def test_dispatcher_emits_adapter_argv(adapter, preset, quality, must_have):
    """Every fake adapter's argv slice surfaces verbatim in the
    composed ffmpeg command, in the documented (flag, value) order."""
    reg_patch, get_patch = _patch_registry(adapter)
    with reg_patch, get_patch:
        cmd = build_ffmpeg_command(_req(adapter.encoder, preset, quality))

    # The harness itself never branches on codec — verify by checking
    # the dispatcher uses exactly the adapter-supplied tokens.
    for flag, value in must_have:
        assert flag in cmd, f"{flag} missing from {cmd}"
        assert (
            cmd[cmd.index(flag) + 1] == value
        ), f"{flag} expected {value!r}, got {cmd[cmd.index(flag) + 1]!r}"

    # And the harness still wraps the input + output the same way.
    assert cmd[0] == "ffmpeg"
    assert "-i" in cmd and cmd[cmd.index("-i") + 1] == "ref.yuv"
    assert cmd[-1] == "out.mp4"


def test_dispatcher_unknown_codec_falls_back_to_x264_shape():
    """Adapter not in the registry → fallback x264 shape.

    Required by the hard rule: 'DO NOT remove fallback for adapters
    that don't ship ffmpeg_codec_args'.
    """
    cmd = build_ffmpeg_command(_req("libxyz_unknown", "medium", 30))
    # Falls back to the legacy `-c:v <encoder> -preset <p> -crf <q>` shape.
    assert "-c:v" in cmd and cmd[cmd.index("-c:v") + 1] == "libxyz_unknown"
    assert cmd[cmd.index("-preset") + 1] == "medium"
    assert cmd[cmd.index("-crf") + 1] == "30"


def test_dispatcher_legacy_adapter_without_ffmpeg_codec_args():
    """Registered adapter that doesn't expose ``ffmpeg_codec_args``
    must still work via the legacy shape."""
    reg_patch, get_patch = _patch_registry(_LegacyNoArgsAdapter())
    with reg_patch, get_patch:
        cmd = build_ffmpeg_command(_req("libx264", "medium", 23))
    assert "-c:v" in cmd and cmd[cmd.index("-c:v") + 1] == "libx264"
    assert "-crf" in cmd and cmd[cmd.index("-crf") + 1] == "23"


# ---------------------------------------------------------------------------
# Version probe selection.
# ---------------------------------------------------------------------------


def test_parse_versions_x264_default_kwarg():
    stderr = "ffmpeg version 6.1.1\nx264 - core 164 r3107\n"
    assert parse_versions(stderr) == ("6.1.1", "libx264-164")
    assert parse_versions(stderr, encoder="libx264") == ("6.1.1", "libx264-164")


def test_parse_versions_x265():
    stderr = "ffmpeg version 7.0\nx265 [info]: HEVC encoder version 3.5+1-asm-disabled\n"
    ffm, enc = parse_versions(stderr, encoder="libx265")
    assert ffm == "7.0"
    assert enc.startswith("libx265-")


def test_parse_versions_svtav1():
    stderr = "ffmpeg version 7.0\nSVT-AV1 ENCODER v1.7.0\n"
    ffm, enc = parse_versions(stderr, encoder="libsvtav1")
    assert ffm == "7.0"
    assert enc == "libsvtav1-1.7.0"


def test_parse_versions_nvenc_token():
    stderr = "ffmpeg version 7.0\nStream #0:0 -> h264_nvenc\n"
    ffm, enc = parse_versions(stderr, encoder="h264_nvenc")
    assert ffm == "7.0"
    assert enc == "h264_nvenc"


def test_parse_versions_unknown_encoder_returns_unknown():
    stderr = "ffmpeg version 7.0\nweird stuff\n"
    ffm, enc = parse_versions(stderr, encoder="some_made_up_codec")
    assert ffm == "7.0"
    assert enc == "unknown"


# ---------------------------------------------------------------------------
# End-to-end smoke against ``run_encode`` with a stubbed runner.
# ---------------------------------------------------------------------------


def test_run_encode_uses_dispatcher_argv_and_parses_versions(tmp_path):
    captured: dict[str, list[str]] = {}

    def fake_runner(cmd, capture_output, text, check):
        captured["cmd"] = list(cmd)
        out = Path(cmd[-1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(b"\x00" * 4096)
        return type(
            "R",
            (),
            {
                "returncode": 0,
                "stdout": "",
                "stderr": "ffmpeg version 7.0\nSVT-AV1 ENCODER v2.1.0\n",
            },
        )()

    reg_patch, get_patch = _patch_registry(_SvtAv1Adapter())
    out_path = tmp_path / "out.mkv"
    req = EncodeRequest(
        source=tmp_path / "ref.yuv",
        width=320,
        height=240,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libsvtav1",
        preset="8",
        crf=35,
        output=out_path,
    )

    with reg_patch, get_patch:
        result = run_encode(req, encoder_runner=fake_runner)

    cmd = captured["cmd"]
    assert "-c:v" in cmd and cmd[cmd.index("-c:v") + 1] == "libsvtav1"
    assert "-svtav1-params" in cmd
    assert result.exit_status == 0
    assert result.encoder_version == "libsvtav1-2.1.0"
    assert result.ffmpeg_version == "7.0"
    assert result.encode_size_bytes == 4096


def test_run_encode_runner_kwarg_alias_still_works(tmp_path):
    """``runner=`` alias is preserved for the existing corpus.py caller."""
    captured: dict[str, list[str]] = {}

    def fake_runner(cmd, capture_output, text, check):
        captured["cmd"] = list(cmd)
        out = Path(cmd[-1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(b"\x00")
        return type(
            "R",
            (),
            {"returncode": 0, "stdout": "", "stderr": "ffmpeg version 7.0\nx264 - core 164 r1\n"},
        )()

    out_path = tmp_path / "x.mp4"
    req = EncodeRequest(
        source=tmp_path / "ref.yuv",
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=out_path,
    )
    res = run_encode(req, runner=fake_runner)
    assert res.exit_status == 0
    assert "libsvtav1" not in captured["cmd"]
    assert captured["cmd"][captured["cmd"].index("-c:v") + 1] == "libx264"
