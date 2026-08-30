# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""libaom-av1 codec adapter.

libaom is Google's reference AV1 encoder. Compared with the SVT-AV1
adapter shipped alongside this one, libaom is meaningfully slower at
matched ``cpu-used`` settings but tends to deliver slightly higher
quality at the same bitrate at the slow end of its preset range
(see AOM benchmark notes referenced from ``docs/usage/vmaf-tune.md``).

The FFmpeg invocation shape is::

    ffmpeg -i src -c:v libaom-av1 -crf <CRF> -cpu-used <CPU_USED> -an -y out.mkv

libaom's quality knob ``-crf`` runs 0..63 (same numeric range as
SVT-AV1, different rate-distortion curve). The speed/quality knob is
``-cpu-used`` 0..9 (0 = slowest/best, 9 = fastest). For interface
parity with x264/x265 we expose human-readable preset names that map
onto the ``cpu-used`` integer; the search loop only ever speaks
``preset`` and ``crf`` and never branches on codec identity.

The adapter is live through the codec-dispatcher path in
``encode.py``: :meth:`ffmpeg_codec_args` emits libaom's
``-cpu-used`` shape rather than the generic ``-preset`` flag, and the
search loop stays codec-agnostic by speaking only ``preset`` and
``crf``.
"""

from __future__ import annotations

import dataclasses
from collections.abc import Mapping
from pathlib import Path
from types import MappingProxyType

from . import _gop_common

# Canonical preset-name -> cpu-used mapping. Frozen via MappingProxyType
# so tests and downstream phases can rely on its identity. The names
# parallel x264's preset vocabulary so a search-loop sweep over
# {x264, x265, svtav1, libaom} can share a preset axis.
_PRESET_CPU_USED: Mapping[str, int] = MappingProxyType(
    {
        "placebo": 0,
        "slowest": 1,
        "slower": 2,
        "slow": 3,
        "medium": 4,
        "fast": 5,
        "faster": 6,
        "veryfast": 7,
        "superfast": 8,
        "ultrafast": 9,
    }
)


@dataclasses.dataclass(frozen=True)
class LibaomAdapter:
    """libaom-av1 single-pass CRF adapter."""

    name: str = "libaom-av1"
    encoder: str = "libaom-av1"
    quality_knob: str = "crf"
    # libaom accepts CRF 0..63; the full window is exposed because the
    # informative VMAF range for AV1 differs from x264 and Phase B
    # bisect needs the headroom. Out-of-range values still raise.
    quality_range: tuple[int, int] = (0, 63)
    quality_default: int = 35
    invert_quality: bool = True  # higher CRF = lower quality

    # Predictor probe-encode knobs. libaom uses cpu-used, not -preset; the
    # canonical preset name maps to cpu-used 9 in _PRESET_CPU_USED.
    probe_preset: str = "ultrafast"
    probe_quality: int = 35
    supports_qpfile: bool = True
    supports_saliency_roi: bool = True
    # ADR-0332: this encoder has no parseable first-pass stats file.
    supports_encoder_stats: bool = False
    # ADR-0546 (Phase F real 2-pass): libaom-av1 honours FFmpeg's generic
    # ``-pass`` / ``-passlogfile`` pair — same mechanism libvpx uses.
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
        """Map a human preset name onto libaom's ``-cpu-used`` integer.

        Raises ``ValueError`` for unknown preset names.
        """
        if preset not in _PRESET_CPU_USED:
            raise ValueError(f"unknown libaom preset {preset!r}; expected one of {self.presets}")
        return _PRESET_CPU_USED[preset]

    def validate(self, preset: str, crf: int) -> None:
        """Raise ``ValueError`` if ``(preset, crf)`` is unsupported."""
        if preset not in self.presets:
            raise ValueError(f"unknown libaom preset {preset!r}; expected one of {self.presets}")
        lo, hi = self.quality_range
        if not lo <= crf <= hi:
            raise ValueError(f"crf {crf} outside libaom range [{lo}, {hi}]")

    def ffmpeg_codec_args(self, preset: str, crf: int) -> list[str]:
        """FFmpeg argv slice for libaom-av1 single-pass CRF.

        libaom uses ``-cpu-used`` (not ``-preset``); the canonical
        preset name is mapped via :data:`_PRESET_CPU_USED`. ``-c:v
        libaom-av1`` is included so the dispatcher (HP-1 / ADR-0326)
        can splice the slice in verbatim, mirroring the contract used
        by every other adapter.
        """
        return [
            "-c:v",
            self.encoder,
            "-cpu-used",
            str(self.cpu_used(preset)),
            "-crf",
            str(crf),
        ]

    def extra_params(self) -> tuple[str, ...]:
        """No additional non-codec argv for libaom-av1."""
        return ()

    def qpfile_from_saliency(
        self,
        block_offsets: object,
        out_path: Path,
        *,
        duration_frames: int = 1,
    ) -> Path:
        """Write the x264-style qpfile consumed by patched FFmpeg libaom."""
        from vmaftune.saliency import write_x264_qpfile

        return write_x264_qpfile(block_offsets, Path(out_path), duration_frames=duration_frames)

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by libaom-av1."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` with comma-separated seconds."""
        return _gop_common.default_force_keyframes_args(timestamps)

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """FFmpeg argv slice for libaom-av1 2-pass encoding (ADR-0546).

        FFmpeg's libaom wrapper routes through the generic ``-pass`` /
        ``-passlogfile`` pair (the same path libvpx uses). The path is
        a *prefix*; FFmpeg writes ``<prefix>-0.log`` as the stream-
        specific sidecar.

        Returns an empty tuple for ``pass_number == 0`` so callers that
        forward this method's result unconditionally don't need a
        single-pass branch.
        """
        if pass_number == 0:
            return ()
        if pass_number not in (1, 2):
            raise ValueError(
                f"libaom-av1 two_pass_args: pass_number must be 1 or 2, got {pass_number}"
            )
        return ("-pass", str(pass_number), "-passlogfile", str(stats_path))

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: fastest cpu-used, fixed CRF.

        libaom does not honour FFmpeg's generic ``-preset`` so the probe
        builds the cpu-used integer directly rather than going through
        ``ffmpeg_codec_args`` (which validates the CRF against the Phase A
        window the probe deliberately exits).
        """
        return [
            "-c:v",
            self.encoder,
            "-cpu-used",
            str(self.cpu_used(self.probe_preset)),
            "-crf",
            str(self.probe_quality),
        ]
