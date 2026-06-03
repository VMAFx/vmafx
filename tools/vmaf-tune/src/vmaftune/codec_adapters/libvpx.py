# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""libvpx-vp9 codec adapter.

FFmpeg exposes VP9 through the ``libvpx-vp9`` encoder.  The adapter keeps
the same user-facing ``preset`` / ``crf`` contract as the other software
encoders while translating those knobs to libvpx's native shape:

    ffmpeg ... -c:v libvpx-vp9 -deadline good -cpu-used <N> -crf <CRF> -b:v 0

``-b:v 0`` is load-bearing for VP9 constant-quality mode; without it,
FFmpeg treats ``-crf`` as a constrained-quality VBR hint.  ``-deadline
good`` keeps the encoder on the offline quality path while still letting
``-cpu-used`` trade speed for compression efficiency.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
from types import MappingProxyType
from typing import Mapping

from . import _gop_common

_PRESET_CPU_USED: Mapping[str, int] = MappingProxyType(
    {
        "placebo": 0,
        "slowest": 0,
        "slower": 1,
        "slow": 2,
        "medium": 3,
        "fast": 4,
        "faster": 5,
        "veryfast": 5,
        "superfast": 5,
        "ultrafast": 5,
    }
)


@dataclasses.dataclass(frozen=True)
class LibvpxVp9Adapter:
    """``libvpx-vp9`` CRF adapter with optional FFmpeg 2-pass mode."""

    name: str = "libvpx-vp9"
    encoder: str = "libvpx-vp9"
    quality_knob: str = "crf"
    quality_range: tuple[int, int] = (0, 63)
    quality_default: int = 32
    invert_quality: bool = True
    adapter_version: str = "1"

    probe_preset: str = "ultrafast"
    probe_quality: int = 32
    supports_qpfile: bool = False
    supports_encoder_stats: bool = False
    supports_two_pass: bool = True

    presets: tuple[str, ...] = (
        "placebo",
        "slowest",
        "slower",
        "slow",
        "medium",
        "fast",
        "faster",
        "veryfast",
        "superfast",
        "ultrafast",
    )

    def cpu_used(self, preset: str) -> int:
        """Map the common preset vocabulary to libvpx ``-cpu-used``."""
        if preset not in _PRESET_CPU_USED:
            raise ValueError(
                f"unknown libvpx-vp9 preset {preset!r}; expected one of {self.presets}"
            )
        return _PRESET_CPU_USED[preset]

    def validate(self, preset: str, crf: int) -> None:
        """Raise ``ValueError`` if ``(preset, crf)`` is unsupported."""
        if preset not in self.presets:
            raise ValueError(
                f"unknown libvpx-vp9 preset {preset!r}; expected one of {self.presets}"
            )
        lo, hi = self.quality_range
        if not lo <= crf <= hi:
            raise ValueError(f"crf {crf} outside libvpx-vp9 range [{lo}, {hi}]")

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        """FFmpeg argv slice for libvpx-vp9 constant-quality VP9."""
        return [
            "-c:v",
            self.encoder,
            "-deadline",
            "good",
            "-cpu-used",
            str(self.cpu_used(preset)),
            "-crf",
            str(quality),
            "-b:v",
            "0",
        ]

    def extra_params(self) -> tuple[str, ...]:
        """Enable VP9 row multithreading on hosts whose libvpx supports it."""
        return ("-row-mt", "1")

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by libvpx-vp9."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` with comma-separated seconds."""
        return _gop_common.default_force_keyframes_args(timestamps)

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: fastest good-deadline VP9 CRF."""
        return self.ffmpeg_codec_args(self.probe_preset, self.probe_quality)

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """FFmpeg argv slice for libvpx-vp9 2-pass encoding.

        FFmpeg's libvpx wrapper uses the generic ``-pass`` /
        ``-passlogfile`` pair.  The path is a prefix; FFmpeg creates the
        concrete ``<prefix>-0.log`` sidecar.
        """
        if pass_number == 0:
            return ()
        if pass_number not in (1, 2):
            raise ValueError(
                f"libvpx-vp9 two_pass_args: pass_number must be 1 or 2, got {pass_number}"
            )
        return ("-pass", str(pass_number), "-passlogfile", str(stats_path))
