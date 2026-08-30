# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Per-adapter live-encode smoke tests for the HP-1 dispatcher pivot.

Phase-A audit item HP-1 (ADR-0326) replaced three hardcode sites that
emitted ``["-c:v", req.encoder, "-preset", req.preset, "-crf",
str(req.crf)]`` with calls to ``adapter.ffmpeg_codec_args(req.preset,
req.crf)``.

Without this pivot 11 of the 16 registered adapters would produce
codec-incorrect argv at run time — most visibly libaom-av1, which uses
``-cpu-used`` rather than ``-preset`` and would have crashed FFmpeg's
argument parser at every call site. This test parametrises across
every registered codec, mocks ``subprocess.run``, captures the argv
the dispatcher composes, and asserts the codec-correct flags are
present.

Subprocess boundary is the integration seam — no live ffmpeg runs.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import codec_adapters as ca
from vmaftune.encode import (
    EncodeRequest,
    build_ffmpeg_command,
    run_encode,
)

# ---------------------------------------------------------------------------
# Per-adapter fixture table.
#
# For every codec in ``codec_adapters._REGISTRY`` we record a known-good
# ``(preset, quality)`` pair plus the codec-correct flag tokens the
# dispatcher MUST emit. The presets are picked from each adapter's
# declared ``presets`` tuple; the quality numbers are inside each
# adapter's accepted ``quality_range`` window.
# ---------------------------------------------------------------------------

_PER_ADAPTER_FIXTURES: tuple[tuple[str, str, int, tuple[tuple[str, str], ...]], ...] = (
    # libx264 — historic shape, must stay byte-for-byte unchanged.
    ("libx264", "medium", 23, (("-c:v", "libx264"), ("-preset", "medium"), ("-crf", "23"))),
    # libx265 — same shape as libx264 (preset + crf).
    ("libx265", "medium", 28, (("-c:v", "libx265"), ("-preset", "medium"), ("-crf", "28"))),
    # libaom-av1 — the canary case: uses -cpu-used, NOT -preset.
    (
        "libaom-av1",
        "medium",
        35,
        (("-c:v", "libaom-av1"), ("-cpu-used", "4"), ("-crf", "35")),
    ),
    # libsvtav1 — preset is an integer string (medium → 7).
    ("libsvtav1", "medium", 35, (("-c:v", "libsvtav1"), ("-preset", "7"), ("-crf", "35"))),
    # libvpx-vp9 — good-deadline CRF mode with VP9's cpu-used knob.
    (
        "libvpx-vp9",
        "medium",
        35,
        (
            ("-c:v", "libvpx-vp9"),
            ("-deadline", "good"),
            ("-cpu-used", "3"),
            ("-crf", "35"),
            ("-b:v", "0"),
        ),
    ),
    # libvvenc — uses -qp, not -crf; preset compresses to native vocab.
    ("libvvenc", "medium", 32, (("-c:v", "libvvenc"), ("-preset", "medium"), ("-qp", "32"))),
    # NVENC family — uses -cq and pN preset names.
    ("h264_nvenc", "medium", 23, (("-c:v", "h264_nvenc"), ("-preset", "p4"), ("-cq", "23"))),
    ("hevc_nvenc", "medium", 23, (("-c:v", "hevc_nvenc"), ("-preset", "p4"), ("-cq", "23"))),
    ("av1_nvenc", "medium", 23, (("-c:v", "av1_nvenc"), ("-preset", "p4"), ("-cq", "23"))),
    # AMF family — no -preset; uses -quality + -rc cqp + -qp_i / -qp_p.
    (
        "h264_amf",
        "medium",
        23,
        (
            ("-c:v", "h264_amf"),
            ("-quality", "balanced"),
            ("-rc", "cqp"),
            ("-qp_i", "23"),
            ("-qp_p", "23"),
        ),
    ),
    (
        "hevc_amf",
        "medium",
        23,
        (
            ("-c:v", "hevc_amf"),
            ("-quality", "balanced"),
            ("-rc", "cqp"),
            ("-qp_i", "23"),
            ("-qp_p", "23"),
        ),
    ),
    (
        "av1_amf",
        "medium",
        23,
        (
            ("-c:v", "av1_amf"),
            ("-quality", "balanced"),
            ("-rc", "cqp"),
            ("-qp_i", "23"),
            ("-qp_p", "23"),
        ),
    ),
    # QSV family — uses -global_quality, not -crf.
    (
        "h264_qsv",
        "medium",
        23,
        (("-c:v", "h264_qsv"), ("-preset", "medium"), ("-global_quality", "23")),
    ),
    (
        "hevc_qsv",
        "medium",
        23,
        (("-c:v", "hevc_qsv"), ("-preset", "medium"), ("-global_quality", "23")),
    ),
    (
        "av1_qsv",
        "medium",
        23,
        (("-c:v", "av1_qsv"), ("-preset", "medium"), ("-global_quality", "23")),
    ),
    # VideoToolbox family — uses -realtime + -q:v (not -preset / -crf).
    (
        "h264_videotoolbox",
        "medium",
        65,
        (("-c:v", "h264_videotoolbox"), ("-realtime", "0"), ("-q:v", "65")),
    ),
    (
        "hevc_videotoolbox",
        "medium",
        65,
        (("-c:v", "hevc_videotoolbox"), ("-realtime", "0"), ("-q:v", "65")),
    ),
    # av1_videotoolbox — placeholder adapter; ffmpeg_codec_args raises until
    # upstream FFmpeg ships AV1 VideoToolbox support (ADR-0339). Listed
    # here so the fixture-coverage gate stays green; the per-adapter command
    # test is expected to raise and is not parametrised on this row.
    (
        "av1_videotoolbox",
        "medium",
        65,
        (),  # no must-have args: adapter raises on ffmpeg_codec_args
    ),
    # prores_videotoolbox — uses -realtime + -profile:v (not -preset / -crf).
    (
        "prores_videotoolbox",
        "ultrafast",
        0,
        (("-c:v", "prores_videotoolbox"), ("-realtime", "1"), ("-profile:v", "proxy")),
    ),
)


def _mk_request(encoder: str, preset: str, quality: int) -> EncodeRequest:
    return EncodeRequest(
        source=Path("ref.yuv"),
        width=320,
        height=240,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder=encoder,
        preset=preset,
        crf=quality,
        output=Path("out.mp4"),
    )


def test_fixture_table_covers_every_registered_adapter() -> None:
    """The fixture table parametrises across every registered adapter.

    HP-1 contract: ``ffmpeg_codec_args`` is a runtime contract for the
    full registry, not a subset. If a new adapter lands without a
    fixture row this test fails loudly so the registry and the smoke
    table stay in lock-step.
    """
    fixture_codecs = {row[0] for row in _PER_ADAPTER_FIXTURES}
    registered = set(ca.known_codecs())
    missing = registered - fixture_codecs
    assert not missing, (
        f"per-adapter fixture table missing rows for: {sorted(missing)}; "
        f"add a row to _PER_ADAPTER_FIXTURES in this file."
    )


@pytest.mark.parametrize(
    ("encoder", "preset", "quality", "must_have"),
    _PER_ADAPTER_FIXTURES,
    ids=[row[0] for row in _PER_ADAPTER_FIXTURES],
)
def test_build_ffmpeg_command_emits_codec_correct_argv(
    encoder: str, preset: str, quality: int, must_have: tuple[tuple[str, str], ...]
) -> None:
    """The dispatcher emits each adapter's codec-correct flag pairs.

    Asserts ``-cpu-used`` shows up for libaom (not ``-preset``),
    ``-cq`` for NVENC (not ``-crf``), ``-global_quality`` for QSV,
    ``-quality + -rc cqp + -qp_i + -qp_p`` for AMF, ``-realtime + -q:v``
    for VideoToolbox, ``-qp`` for VVenC. x264 / x265 keep their
    historic ``-preset + -crf`` shape.
    """
    # av1_videotoolbox raises until upstream FFmpeg ships AV1 VT support.
    if encoder == "av1_videotoolbox":
        pytest.xfail("av1_videotoolbox raises Av1VideoToolboxUnavailableError (ADR-0339)")
    cmd = build_ffmpeg_command(_mk_request(encoder, preset, quality))
    for flag, value in must_have:
        assert flag in cmd, f"{flag} missing from {cmd}"
        idx = cmd.index(flag)
        assert cmd[idx + 1] == value, f"{flag} expected {value!r}, got {cmd[idx + 1]!r}"


@pytest.mark.parametrize(
    ("encoder", "preset", "quality", "must_have"),
    _PER_ADAPTER_FIXTURES,
    ids=[row[0] for row in _PER_ADAPTER_FIXTURES],
)
def test_run_encode_passes_codec_correct_argv_to_subprocess(
    tmp_path: Path,
    encoder: str,
    preset: str,
    quality: int,
    must_have: tuple[tuple[str, str], ...],
) -> None:
    """End-to-end smoke: the captured subprocess argv contains the
    codec-correct flag pairs, with no live ffmpeg invocation.

    Mocks ``subprocess.run`` via the ``runner=`` injection seam used
    elsewhere in the corpus pipeline (``encode.run_encode``).
    """
    if encoder == "av1_videotoolbox":
        pytest.xfail("av1_videotoolbox raises Av1VideoToolboxUnavailableError (ADR-0339)")
    captured: dict[str, list[str]] = {}

    def fake_runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
        # Capture only the first call — the actual encode argv. The
        # encoder-version probe (``ffmpeg -version``) may fire as a second
        # call for codecs in ``_VERSION_PROBE_PATTERNS`` (ADR-0498 follow-
        # up #7), but we only care about the encode argv here.
        if "cmd" not in captured:
            captured["cmd"] = list(cmd)
        # Touch the output so run_encode's size measurement returns >0.
        if cmd[-1] not in ("-version", "null"):
            out = Path(cmd[-1])
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(b"\x00")
        return type("R", (), {"returncode": 0, "stdout": "", "stderr": "ffmpeg version 7.0\n"})()

    out_path = tmp_path / "out.mp4"
    req = EncodeRequest(
        source=tmp_path / "ref.yuv",
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder=encoder,
        preset=preset,
        crf=quality,
        output=out_path,
    )
    result = run_encode(req, runner=fake_runner)
    assert result.exit_status == 0

    cmd = captured["cmd"]
    for flag, value in must_have:
        assert flag in cmd, f"{flag} missing from captured argv {cmd}"
        idx = cmd.index(flag)
        assert cmd[idx + 1] == value, f"{flag} expected {value!r}, got {cmd[idx + 1]!r}"


def test_libaom_argv_does_not_contain_preset_flag() -> None:
    """Regression: libaom-av1 must NOT emit a ``-preset`` token.

    Pre-HP-1 the hardcode in ``encode.build_ffmpeg_command`` emitted
    ``-preset <name>`` for every codec, which would have caused libaom
    to reject the argument at runtime (libaom uses ``-cpu-used``).
    """
    cmd = build_ffmpeg_command(_mk_request("libaom-av1", "medium", 35))
    # The harness itself uses no top-level ``-preset`` for libaom; the
    # only ``-preset`` token in the legacy hardcode would have come from
    # the codec slice. With the dispatcher pivot it must be absent.
    codec_slice_start = cmd.index("-c:v") + 2
    codec_slice_end = cmd.index(str(Path("out.mp4")))
    codec_slice = cmd[codec_slice_start:codec_slice_end]
    assert (
        "-preset" not in codec_slice
    ), f"libaom-av1 argv must not carry -preset; got {codec_slice}"
    assert "-cpu-used" in codec_slice


def test_x264_argv_byte_for_byte_legacy_shape() -> None:
    """libx264 dispatcher argv equals the pre-HP-1 hardcode shape.

    The HP-1 contract pivot is intentionally a no-op for x264 — the
    legacy hardcode and ``X264Adapter.ffmpeg_codec_args`` must produce
    identical token sequences. This test pins that property.
    """
    cmd = build_ffmpeg_command(_mk_request("libx264", "medium", 23))
    legacy = ["-c:v", "libx264", "-preset", "medium", "-crf", "23"]
    # The legacy slice appears as a contiguous run inside the full argv.
    start = cmd.index("-c:v")
    assert cmd[start : start + len(legacy)] == legacy


def test_x265_argv_byte_for_byte_legacy_shape() -> None:
    """libx265 dispatcher argv equals the pre-HP-1 hardcode shape."""
    cmd = build_ffmpeg_command(_mk_request("libx265", "medium", 28))
    legacy = ["-c:v", "libx265", "-preset", "medium", "-crf", "28"]
    start = cmd.index("-c:v")
    assert cmd[start : start + len(legacy)] == legacy
