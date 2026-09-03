# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""libx264 codec adapter — Phase A + Phase F.

CRF-mode encodes; the eight standard x264 presets; quality range
pinned to the canonical CRF window for which VMAF is informative.
Phase F opts into FFmpeg's native two-pass controls via ``-pass`` and
``-passlogfile``.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path

from . import _gop_common


@dataclasses.dataclass(frozen=True)
class X264Adapter:
    """libx264 CRF adapter with optional FFmpeg-native two-pass."""

    name: str = "libx264"
    encoder: str = "libx264"
    quality_knob: str = "crf"
    # Bumps when the adapter's argv shape / preset list / CRF window
    # changes. Folded into the cache key so an adapter upgrade
    # invalidates older entries (ADR-0298).
    adapter_version: str = "2"
    # x264 nominally accepts 0..51 and that's the search domain for
    # ADR-0306 coarse-to-fine. The perceptually-informative window
    # for the recommend / target-VMAF flows is narrower (~15..40
    # in practice) but we let the search loop discover that rather
    # than refuse out-of-band candidates up front. ADR-0237 Phase A
    # grid generation also drives off this field.
    quality_range: tuple[int, int] = (0, 51)
    quality_default: int = 23
    invert_quality: bool = True  # higher CRF = lower quality

    # Predictor probe-encode knobs (see _gop_common docstring).
    probe_preset: str = "ultrafast"
    probe_quality: int = 28
    # libx264 honours --qpfile via FFmpeg's -x264-params, so the saliency
    # QP-offset map (saliency.py) drives x264 directly.
    supports_qpfile: bool = True
    # ADR-0332: libx264 emits per-frame pass-1 stats via
    # ``-pass 1 -passlogfile <prefix>``; the parser is in
    # :mod:`vmaftune.encoder_stats`.
    supports_encoder_stats: bool = True
    # Phase F (ADR-0333): libx264 supports 2-pass encoding through
    # FFmpeg's native ``-pass`` / ``-passlogfile`` pair.
    supports_two_pass: bool = True

    presets: tuple[str, ...] = (
        "ultrafast",
        "superfast",
        "veryfast",
        "faster",
        "fast",
        "medium",
        "slow",
        "slower",
        "veryslow",
    )

    def validate(self, preset: str, crf: int) -> None:
        """Raise ``ValueError`` if ``(preset, crf)`` is unsupported."""
        if preset not in self.presets:
            raise ValueError(f"unknown x264 preset {preset!r}; expected one of {self.presets}")
        lo, hi = self.quality_range
        if not lo <= crf <= hi:
            raise ValueError(f"crf {crf} outside Phase A range [{lo}, {hi}]")

    def ffmpeg_codec_args(self, preset: str, quality: int, pass_number: int = 0) -> list[str]:
        """FFmpeg argv slice for libx264.

        Adapter-contract entry point used by the codec-agnostic
        dispatcher (ADR-0294). Identical to the legacy hard-coded
        x264 path: ``-c:v libx264 -preset <p> -crf <q>`` on single-pass
        (pass_number == 0). In 2-pass mode (pass_number in (1, 2)), omits
        -crf to avoid conflicting rate-control flags (T-VMAFTUNE-TWOPASS-CRF-INVALID-2026-08-30).
        """
        if pass_number != 0:
            return ["-c:v", self.encoder, "-preset", preset]
        return ["-c:v", self.encoder, "-preset", preset, "-crf", str(quality)]

    def extra_params(self) -> tuple[str, ...]:
        """Additional ffmpeg argv slices (none for libx264 Phase A)."""
        return ()

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by libx264 verbatim."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` with comma-separated seconds."""
        return _gop_common.default_force_keyframes_args(timestamps)

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: ultrafast preset, fixed CRF."""
        return _gop_common.default_probe_args(self)

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """FFmpeg argv slice for libx264 2-pass encoding.

        FFmpeg exposes x264 pass control through the generic
        ``-pass`` / ``-passlogfile`` flags. ``stats_path`` is used as
        the passlog prefix; FFmpeg writes stream-specific files under
        that prefix while pass 1 analyses the source and pass 2 reads
        the recorded rate-control state.

        Returns an empty tuple when ``pass_number == 0`` so callers
        can forward the result on single-pass paths without branching.
        Raises :class:`ValueError` for pass numbers outside ``{1, 2}``.
        """
        if pass_number == 0:
            return ()
        if pass_number not in (1, 2):
            raise ValueError(
                f"libx264 two_pass_args: pass_number must be 1 or 2, got {pass_number}"
            )
        return ("-pass", str(pass_number), "-passlogfile", str(stats_path))
