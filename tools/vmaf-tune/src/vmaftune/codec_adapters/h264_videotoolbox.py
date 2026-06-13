# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Apple VideoToolbox H.264 codec adapter.

FFmpeg encoder name: ``h264_videotoolbox``. Hardware-accelerated H.264
encoding on Apple Silicon (M-series) and Intel Macs with a T2 chip.
See ``_videotoolbox_common.py`` for the shared quality/preset mapping
and the rationale for using ``-q:v`` (range 0..100, higher = better)
instead of ``-crf``.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path

from . import _gop_common
from ._videotoolbox_common import (
    VIDEOTOOLBOX_PRESETS,
    VIDEOTOOLBOX_QUALITY_DEFAULT,
    VIDEOTOOLBOX_QUALITY_RANGE,
    validate_videotoolbox,
    videotoolbox_two_pass_args,
)

_PRESET_TO_REALTIME = {
    "ultrafast": 1,
    "superfast": 1,
    "veryfast": 1,
    "faster": 1,
    "fast": 1,
    "medium": 0,
    "slow": 0,
    "slower": 0,
    "veryslow": 0,
}


@dataclasses.dataclass(frozen=True)
class H264VideoToolboxAdapter:
    """``h264_videotoolbox`` adapter — hardware H.264 on Apple Silicon."""

    name: str = "h264_videotoolbox"
    encoder: str = "h264_videotoolbox"
    quality_knob: str = "q:v"
    # Bumps when the adapter's argv shape / preset list / quality
    # window changes (ADR-0298 cache key).
    adapter_version: str = "1"
    quality_range: tuple[int, int] = VIDEOTOOLBOX_QUALITY_RANGE
    quality_default: int = VIDEOTOOLBOX_QUALITY_DEFAULT
    invert_quality: bool = False  # higher q:v = higher quality

    # Predictor probe-encode knobs. VideoToolbox has only realtime/non-
    # realtime; "ultrafast" maps to realtime=1 (the fast probe).
    probe_preset: str = "ultrafast"
    probe_quality: int = 60
    supports_qpfile: bool = False
    # ADR-0332: this encoder has no parseable first-pass stats file.
    supports_encoder_stats: bool = False
    # ADR-0546: VideoToolbox is single-pass only — the underlying
    # ``VTCompressionSession`` C API has no multi-pass interface.
    # :meth:`two_pass_args` always raises so callers fail loud rather
    # than producing a silently-wrong bitstream.
    supports_two_pass: bool = False

    presets: tuple[str, ...] = VIDEOTOOLBOX_PRESETS

    def validate(self, preset: str, crf: int) -> None:
        """Raise ``ValueError`` if ``(preset, q:v)`` is unsupported.

        The ``crf`` parameter carries the harness's quality value; for
        VideoToolbox this is the ``-q:v`` integer on the [0, 100] axis.
        """
        validate_videotoolbox(preset, crf)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        """FFmpeg argv slice for ``h264_videotoolbox``.

        Maps the nine-name preset onto VT's binary ``-realtime`` flag.
        """
        realtime = _PRESET_TO_REALTIME[preset]
        return [
            "-c:v",
            self.encoder,
            "-realtime",
            str(realtime),
            "-q:v",
            str(quality),
        ]

    def extra_params(self) -> tuple[str, ...]:
        """No additional non-codec argv for VideoToolbox."""
        return ()

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by VideoToolbox."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` with comma-separated seconds."""
        return _gop_common.default_force_keyframes_args(timestamps)

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: realtime mode + middle q:v."""
        return _gop_common.default_probe_args(self)

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """Always raise — VideoToolbox is single-pass only (ADR-0546)."""
        return videotoolbox_two_pass_args(self.encoder, pass_number, stats_path)
