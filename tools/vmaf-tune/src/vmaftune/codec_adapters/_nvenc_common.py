# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared helpers for the NVIDIA NVENC family of codec adapters.

Hardware encoders share a common preset / quality vocabulary that
differs from libx264's. Centralising it here keeps the per-codec files
(``h264_nvenc``, ``hevc_nvenc``, ``av1_nvenc``) thin and forces a
single source of truth for the preset map and the constant-quality
knob range.

NVENC presets are named ``p1`` (fastest) through ``p7`` (slowest, best
quality). The fork's codec-adapter contract surfaces the libx264-style
mnemonic preset names (``ultrafast``...``placebo``); this module owns
the canonical mnemonic-to-NVENC mapping so all three NVENC codecs
agree.

``BaseNvencAdapter`` is a frozen dataclass that supplies the full
method body shared by all three per-codec adapters. Each per-codec
class inherits from it and overrides only the ``name`` / ``encoder``
fields.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Final

from . import _gop_common

# Canonical NVENC quality knob is constant-quantizer (``-cq``); the
# valid integer range is 0..51, mirroring x264 CRF semantics. We
# surface the same perceptually-informative window as the libx264
# adapter for cross-codec corpus consistency.
NVENC_CQ_RANGE: Final[tuple[int, int]] = (15, 40)

# Full hardware-allowed CQ window — used for the validate path; the
# Phase A grid stays inside ``NVENC_CQ_RANGE`` but adapters reject
# anything outside the hardware window outright.
NVENC_CQ_HARD_LIMITS: Final[tuple[int, int]] = (0, 51)

# Mnemonic preset names accepted by every NVENC adapter, ordered fast
# to slow. ``placebo`` is included as an alias of ``p7`` so callers
# can reuse libx264-style sweeps unchanged; NVENC has no slower mode.
NVENC_PRESETS: Final[tuple[str, ...]] = (
    "ultrafast",
    "superfast",
    "veryfast",
    "faster",
    "fast",
    "medium",
    "slow",
    "slower",
    "slowest",
    "placebo",
)

# Mnemonic → NVENC ``-preset pN`` mapping. NVENC has 7 levels; the
# 10 mnemonic names collapse onto them per the comment in
# ``codec_adapters/__init__.py``. The convention is to clamp the fast
# end at p1 and the slow end at p7.
_NVENC_PRESET_MAP: Final[dict[str, str]] = {
    "ultrafast": "p1",
    "superfast": "p1",
    "veryfast": "p1",
    "faster": "p2",
    "fast": "p3",
    "medium": "p4",
    "slow": "p5",
    "slower": "p6",
    "slowest": "p7",
    "placebo": "p7",
}


def nvenc_preset(name: str) -> str:
    """Translate a mnemonic preset name to an NVENC ``pN`` string.

    Raises ``ValueError`` if the name is not recognised. Pure
    function — no I/O — so tests can pin the mapping.
    """
    if name not in _NVENC_PRESET_MAP:
        raise ValueError(f"unknown NVENC preset {name!r}; expected one of {NVENC_PRESETS}")
    return _NVENC_PRESET_MAP[name]


def validate_nvenc(preset: str, cq: int) -> None:
    """Common validation for any NVENC adapter.

    Rejects an unknown preset name or a ``-cq`` value outside the
    hardware window ``[0, 51]``. Out-of-Phase-A-window values
    (``< 15`` or ``> 40``) are accepted here — the adapter callers
    decide how strict they want to be — but kept available via
    ``NVENC_CQ_RANGE`` for grid generation.
    """
    if preset not in NVENC_PRESETS:
        raise ValueError(f"unknown NVENC preset {preset!r}; expected one of {NVENC_PRESETS}")
    lo, hi = NVENC_CQ_HARD_LIMITS
    if not lo <= cq <= hi:
        raise ValueError(f"cq {cq} outside NVENC range [{lo}, {hi}]")


@dataclasses.dataclass(frozen=True)
class BaseNvencAdapter:
    """Shared implementation for the NVENC H.264 / HEVC / AV1 adapters.

    Subclasses override only the two codec-identity fields (``name`` and
    ``encoder``). All method bodies are identical across the three
    per-codec adapters — they live here so the per-codec files each
    collapse to ~15 LOC of field declarations.
    """

    name: str = "h264_nvenc"
    encoder: str = "h264_nvenc"
    quality_knob: str = "cq"
    quality_range: tuple[int, int] = NVENC_CQ_RANGE
    quality_default: int = 23
    invert_quality: bool = True  # higher CQ = lower quality

    # Predictor probe-encode knobs. "ultrafast" maps to NVENC ``p1``.
    probe_preset: str = "ultrafast"
    probe_quality: int = 28
    supports_qpfile: bool = False
    # ADR-0332: hardware encoders have no parseable first-pass stats file.
    supports_encoder_stats: bool = False
    # ADR-0546: NVENC's "two-pass" is a single-invocation in-encoder
    # analysis pre-pass (``-multipass fullres``), not a true ffmpeg
    # two-invocation 2-pass with a stats sidecar. The 2-pass driver in
    # :func:`vmaftune.encode.run_two_pass_encode` therefore leaves
    # ``supports_two_pass = False`` and falls back to single-pass; the
    # caller can still call :meth:`two_pass_args` directly to retrieve
    # the hardware-quality-boost flags for an enhanced single-pass run.
    supports_two_pass: bool = False

    presets: tuple[str, ...] = NVENC_PRESETS

    def validate(self, preset: str, cq: int) -> None:
        """Raise ``ValueError`` if ``(preset, cq)`` is unsupported."""
        validate_nvenc(preset, cq)

    def nvenc_preset(self, preset: str) -> str:
        """Translate a mnemonic preset to its NVENC ``pN`` name."""
        return nvenc_preset(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        """FFmpeg argv slice for constant-quality NVENC encode.

        NVENC uses ``-cq`` (not ``-crf``) and its native ``pN`` preset
        token; :func:`nvenc_preset` collapses the canonical mnemonic
        onto NVENC's seven levels.
        """
        return [
            "-c:v",
            self.encoder,
            "-preset",
            self.nvenc_preset(preset),
            "-cq",
            str(quality),
        ]

    def extra_params(self) -> tuple[str, ...]:
        """No additional non-codec argv for NVENC encoders."""
        return ()

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by NVENC."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` plus NVENC's ``-forced-idr 1``.

        NVENC honours ``-force_key_frames`` only when ``-forced-idr 1`` is
        also set; otherwise the encoder may emit non-IDR keyframes that
        downstream players treat as non-seekable.
        """
        if not timestamps:
            return ()
        return _gop_common.default_force_keyframes_args(timestamps) + ("-forced-idr", "1")

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """NVENC hardware multipass argv (ADR-0546).

        NVENC implements "two-pass" as an in-encoder full-resolution
        analysis stage inside a single ffmpeg invocation — there is no
        separate first-pass stats file written to disk. The relevant
        FFmpeg flag is ``-multipass fullres`` (the slow / high-quality
        variant; ``qres`` is the quarter-resolution fast variant).

        Because the analysis runs inside one ffmpeg call, the per-pass
        contract differs from the software encoders:

        - ``pass_number == 0`` → empty tuple (single-pass).
        - ``pass_number == 1`` → ``("-multipass", "fullres")`` so the
          driver can fold the flag in even if it is otherwise driving
          the two-pass loop.
        - ``pass_number == 2`` → empty tuple (the multipass analysis
          already ran inside pass 1's single invocation; a second
          ffmpeg invocation would re-encode from scratch).

        The adapter declares ``supports_two_pass = False`` so the
        :func:`vmaftune.encode.run_two_pass_encode` driver will not
        attempt to split the work across two invocations; callers that
        want the hardware quality boost append this method's pass-1
        return value to a single-pass ``EncodeRequest.extra_params``.

        ``stats_path`` is accepted for interface uniformity with the
        software adapters but unused — NVENC keeps its analysis state
        in VRAM across the single invocation.
        """
        del stats_path  # NVENC's analysis is in-VRAM, no disk sidecar
        if pass_number == 0:
            return ()
        if pass_number == 1:
            return ("-multipass", "fullres")
        if pass_number == 2:
            return ()
        raise ValueError(f"NVENC two_pass_args: pass_number must be 0, 1, or 2, got {pass_number}")

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: NVENC ``p1`` preset, fixed CQ."""
        return [
            "-c:v",
            self.encoder,
            "-preset",
            self.nvenc_preset(self.probe_preset),
            "-cq",
            str(self.probe_quality),
        ]


__all__ = [
    "BaseNvencAdapter",
    "NVENC_CQ_HARD_LIMITS",
    "NVENC_CQ_RANGE",
    "NVENC_PRESETS",
    "nvenc_preset",
    "validate_nvenc",
]
