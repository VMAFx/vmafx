# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""RFC 6381 codec strings of a ladder rendition, read from a real encode.

The HLS ``CODECS`` attribute and the DASH ``codecs`` attribute name the
codec, profile and level a player must support. They used to be the
constant ``avc1.640028`` (H.264 High, level 4.0) whatever the encoder.
:func:`probe_codec_string` encodes two frames at the rendition's
geometry and frame rate with the adapter's own argv, writes them as
MP4, and builds the string from the codec configuration record the
muxer stored (``avcC``, ``hvcC``, ``av1C``, ``vpcC``), so the profile
and level are the ones the encoder chose:

- H.264: ``avc1.PPCCLL`` (RFC 6381 §3.3): profile, constraint flags and
  level bytes of the ``avcC`` record.
- HEVC: ``hvc1.<space><profile>.<compat>.<tier><level>[.<constraint>]``
  (ISO/IEC 14496-15 Annex E): compatibility flags in reverse bit order,
  trailing zero constraint bytes dropped. ``hvc1`` is the sample entry
  Apple HLS requires; package HEVC renditions with ``-tag:v hvc1``.
- AV1: ``av01.<profile>.<level><tier>.<bitDepth>`` (AV1 ISOBMFF binding
  §5, mandatory fields only).
- VP9: ``vp09.<profile>.<level>.<bitDepth>`` (VP9 ISOBMFF binding). The
  VP9 bitstream signals no level and the muxer's guess depends on the
  frame rate it sees in a two-frame file, so the level comes from the
  VP9 level table (https://www.webmproject.org/vp9/levels/) for the
  rendition's geometry, frame rate and bitrate.

Codecs without one of these records (VVC, ProRes) raise
:class:`CodecStringError`: a manifest that names the wrong codec is
worse than none.
"""

from __future__ import annotations

import subprocess
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Any

_CONFIG_BOXES: tuple[bytes, ...] = (b"avcC", b"hvcC", b"av1C", b"vpcC")

# VP9 levels: (level x 10, max luma sample rate, max luma picture size,
# max width or height, max bitrate in kbit/s), from
# https://www.webmproject.org/vp9/levels/ (checked 2026-10-04).
_VP9_LEVELS: tuple[tuple[int, int, int, int, int], ...] = (
    (10, 829_440, 36_864, 512, 200),
    (11, 2_764_800, 73_728, 768, 800),
    (20, 4_608_000, 122_880, 960, 1_800),
    (21, 9_216_000, 245_760, 1_344, 3_600),
    (30, 20_736_000, 552_960, 2_048, 7_200),
    (31, 36_864_000, 983_040, 2_752, 12_000),
    (40, 83_558_400, 2_228_224, 4_160, 18_000),
    (41, 160_432_128, 2_228_224, 4_160, 30_000),
    (50, 311_951_360, 8_912_896, 8_384, 60_000),
    (51, 588_251_136, 8_912_896, 8_384, 120_000),
    (52, 1_176_502_272, 8_912_896, 8_384, 180_000),
    (60, 1_176_502_272, 35_651_584, 16_832, 180_000),
    (61, 2_353_004_544, 35_651_584, 16_832, 240_000),
    (62, 4_706_009_088, 35_651_584, 16_832, 480_000),
)


class CodecStringError(RuntimeError):
    """The codec string of a rendition cannot be derived."""


def codec_string_from_mp4(
    data: bytes, *, width: int, height: int, framerate: float, bitrate_kbps: float = 0.0
) -> str:
    """Build the codec string from an MP4's codec configuration record.

    ``width`` / ``height`` / ``framerate`` / ``bitrate_kbps`` describe
    the rendition; only the VP9 level uses them.
    """
    fourcc, record = _config_record(data)
    if fourcc == b"avcC":
        return _avc_string(record)
    if fourcc == b"hvcC":
        return _hevc_string(record)
    if fourcc == b"av1C":
        return _av1_string(record)
    return _vp9_string(record, width, height, framerate, bitrate_kbps)


def _config_record(data: bytes) -> tuple[bytes, bytes]:
    """Return ``(fourcc, payload)`` of the first codec configuration box."""
    found: list[tuple[int, bytes, bytes]] = []
    for fourcc in _CONFIG_BOXES:
        at = data.find(fourcc)
        if at < 4:
            continue
        size = int.from_bytes(data[at - 4 : at], "big")
        if size < 12 or at - 4 + size > len(data):
            continue
        found.append((at, fourcc, data[at + 4 : at - 4 + size]))
    if not found:
        raise CodecStringError(
            "the encode carries no avcC, hvcC, av1C or vpcC record; HLS and DASH "
            "manifests can name H.264, HEVC, AV1 and VP9 renditions only"
        )
    _at, fourcc, payload = min(found)
    return fourcc, payload


def _avc_string(record: bytes) -> str:
    """``avc1.PPCCLL`` from an AVCDecoderConfigurationRecord."""
    return f"avc1.{record[1]:02x}{record[2]:02x}{record[3]:02x}"


def _hevc_string(record: bytes) -> str:
    """``hvc1.…`` from an HEVCDecoderConfigurationRecord (ISO/IEC 14496-15 E.3)."""
    space = "" if record[1] >> 6 == 0 else "ABC"[(record[1] >> 6) - 1]
    tier = "H" if (record[1] >> 5) & 1 else "L"
    profile_idc = record[1] & 0x1F
    compat = int.from_bytes(record[2:6], "big")
    reversed_compat = int(f"{compat:032b}"[::-1], 2)
    constraint = bytes(record[6:12]).rstrip(b"\x00")
    tail = "".join(f".{b:X}" for b in constraint)
    return f"hvc1.{space}{profile_idc}.{reversed_compat:X}.{tier}{record[12]}{tail}"


def _av1_string(record: bytes) -> str:
    """``av01.P.LLT.DD`` from an AV1CodecConfigurationRecord."""
    profile = record[1] >> 5
    level = record[1] & 0x1F
    tier = "H" if record[2] >> 7 else "M"
    high_bitdepth = (record[2] >> 6) & 1
    twelve_bit = (record[2] >> 5) & 1
    bitdepth = (
        12 if (profile == 2 and high_bitdepth and twelve_bit) else (10 if high_bitdepth else 8)
    )
    return f"av01.{profile}.{level:02d}{tier}.{bitdepth:02d}"


def _vp9_string(
    record: bytes, width: int, height: int, framerate: float, bitrate_kbps: float
) -> str:
    """``vp09.PP.LL.DD``: profile and bit depth from vpcC, level from the table."""
    profile = record[4]
    bitdepth = record[6] >> 4
    return (
        f"vp09.{profile:02d}.{vp9_level(width, height, framerate, bitrate_kbps):02d}.{bitdepth:02d}"
    )


def vp9_level(width: int, height: int, framerate: float, bitrate_kbps: float = 0.0) -> int:
    """Smallest VP9 level (x 10) whose limits hold the rendition."""
    picture = width * height
    rate = picture * framerate
    for level, max_rate, max_picture, max_side, max_kbps in _VP9_LEVELS:
        if (
            rate <= max_rate
            and picture <= max_picture
            and max(width, height) <= max_side
            and bitrate_kbps <= max_kbps
        ):
            return level
    raise CodecStringError(
        f"no VP9 level holds {width}x{height} at {framerate:g} fps and {bitrate_kbps:g} kbit/s"
    )


def probe_codec_string(
    encoder: str,
    *,
    preset: str,
    quality: int,
    width: int,
    height: int,
    framerate: float,
    pix_fmt: str = "yuv420p",
    bitrate_kbps: float = 0.0,
    ffmpeg_bin: str = "ffmpeg",
    runner: Callable[..., Any] | None = None,
) -> str:
    """Encode two frames like a rendition of ``encoder`` and return its codec string.

    The argv is the adapter's (device chain and upload filter included,
    through :func:`vmaftune.encode.build_ffmpeg_command`'s helpers), so
    the encoder chooses the profile and level a full encode at this
    geometry and frame rate signals. Raises :class:`CodecStringError`
    when the probe encode fails or the codec has no supported record.
    """
    with tempfile.TemporaryDirectory(prefix="vmaftune-codecs-") as tmp:
        out = Path(tmp) / "probe.mp4"
        argv = probe_argv(
            encoder, preset, quality, width, height, framerate, pix_fmt, out, ffmpeg_bin
        )
        run = runner or subprocess.run
        completed = run(argv, capture_output=True, text=True, check=False)
        if int(getattr(completed, "returncode", 1)) != 0 or not out.exists():
            tail = (getattr(completed, "stderr", "") or "").strip().splitlines()[-1:]
            raise CodecStringError(
                f"codec-string probe encode with {encoder} failed: {' '.join(tail) or 'no stderr'}"
            )
        data = out.read_bytes()
    return codec_string_from_mp4(
        data, width=width, height=height, framerate=framerate, bitrate_kbps=bitrate_kbps
    )


def probe_argv(
    encoder: str,
    preset: str,
    quality: int,
    width: int,
    height: int,
    framerate: float,
    pix_fmt: str,
    out: Path,
    ffmpeg_bin: str = "ffmpeg",
) -> list[str]:
    """The ffmpeg argv of the two-frame codec-string probe."""
    from .encode import (
        EncodeRequest,
        _hw_pre_input_args,
        _hw_upload_filter,
        _registered_adapter,
        _resolve_codec_args,
        with_upload_filter,
    )

    req = EncodeRequest(
        source=Path("testsrc2"),
        width=width,
        height=height,
        pix_fmt=pix_fmt,
        framerate=framerate,
        encoder=encoder,
        preset=preset,
        crf=quality,
        output=out,
    )
    adapter = _registered_adapter(encoder)
    return [
        ffmpeg_bin,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        *_hw_pre_input_args(adapter, req),
        "-f",
        "lavfi",
        "-i",
        # Frames enter in the source pixel format, as a raw-YUV source's
        # do; the encoder (or the QSV upload chain) converts from there.
        f"testsrc2=size={width}x{height}:rate={framerate:g},format={pix_fmt}",
        "-frames:v",
        "2",
        *_resolve_codec_args(req),
        *with_upload_filter((), _hw_upload_filter(adapter, req)),
        "-movflags",
        "+faststart",
        "-f",
        "mp4",
        str(out),
    ]


__all__ = [
    "CodecStringError",
    "codec_string_from_mp4",
    "probe_argv",
    "probe_codec_string",
    "vp9_level",
]
