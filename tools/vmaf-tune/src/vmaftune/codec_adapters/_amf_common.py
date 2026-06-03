# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared logic for AMD AMF (Advanced Media Framework) hardware encoders.

AMF is AMD's video-encoding API exposed by FFmpeg through three
encoder names: ``h264_amf``, ``hevc_amf``, and ``av1_amf`` (the last
of which requires RDNA3 silicon, i.e. Radeon RX 7000 series and
newer). Per ADR-0282 the three adapters share a common base because
their CLI shape is identical at the FFmpeg level — only the encoder
binding name differs.

Two AMF specifics shape this module:

1. **Quality is coarser than NVENC / QSV.** AMF exposes only three
   ``-quality`` levels — ``quality``, ``balanced``, ``speed`` —
   versus the seven typical x264 / NVENC / QSV preset names. We map
   the seven canonical preset names down onto the three AMF rungs;
   the mapping is opinionated and documented in ``_PRESET_TO_AMF``.
2. **Rate control is constant-QP.** AMF ``-rc cqp`` plus
   ``-qp_i`` / ``-qp_p`` is the closest analogue to x264 CRF. Range
   is 0..51 (matching H.264 / HEVC quantiser space). The Phase A
   harness window of (15, 40) is preserved for cross-codec
   comparability with x264.

The adapter does not check for runtime AMF availability — that is
handled by ``ensure_amf_available`` which probes ``ffmpeg
-hide_banner -encoders`` and is exercised in the unit tests via a
mocked subprocess runner.

**Linux device init and gfx1036 (AMD Raphael / Phoenix APUs):**
AMF on Linux auto-detects the first AMD DRI render node via the AMF
runtime loader — no explicit ``-init_hw_device`` flags are required at
the FFmpeg layer. However, not all AMD APUs include a VCE (Video Codec
Engine) encode block. The gfx1036 iGPU in AMD Raphael / Phoenix-series
APUs (Ryzen 7000 integrated graphics) is a decoder-only ASIC; it does
not expose an AMF encoder. As a result ``h264_amf``, ``hevc_amf``, and
``av1_amf`` will fail at the probe stage with ``AMF_NOT_SUPPORTED``
even when the AMF runtime and ROCm libraries are installed. This is a
hardware limitation, not a software configuration problem. The
encoder-probe in ``compare.probe_encoder_available`` surfaces a
``hardware encoder not available: … dummy encode failed`` row rather
than aborting the sweep. See ADR-0601 (Bug V14-C).
"""

from __future__ import annotations

import dataclasses
import subprocess
from collections.abc import Callable
from pathlib import Path
from typing import Final

from . import _gop_common

# Canonical 7-level preset vocabulary used by x264 / NVENC / QSV.
# Mapped to AMF's 3 quality levels as follows:
#   placebo / slowest / slower / slow -> quality (slowest, best quality)
#   medium                            -> balanced (default)
#   fast / faster / veryfast / superfast / ultrafast -> speed (fastest)
# The 7-into-3 compression is intentional: AMF's hardware pipeline
# does not expose finer steps. Callers that need finer granularity
# should pin ``-qp_i`` / ``-qp_p`` (the quality knob) instead of the
# preset.
_PRESET_TO_AMF: Final[dict[str, str]] = {
    "placebo": "quality",
    "slowest": "quality",
    "slower": "quality",
    "slow": "quality",
    "medium": "balanced",
    "fast": "speed",
    "faster": "speed",
    "veryfast": "speed",
    "superfast": "speed",
    "ultrafast": "speed",
}

# The three AMF quality rungs in slow->fast order.
_AMF_QUALITIES: Final[tuple[str, ...]] = ("quality", "balanced", "speed")


def map_preset_to_amf_quality(preset: str) -> str:
    """Compress a 7-level preset name onto AMF's 3 quality rungs.

    Raises ``ValueError`` for unknown preset names so the search
    loop fails loudly rather than silently substituting ``balanced``.
    """
    if preset not in _PRESET_TO_AMF:
        raise ValueError(f"unknown AMF preset {preset!r}; expected one of {sorted(_PRESET_TO_AMF)}")
    return _PRESET_TO_AMF[preset]


def ensure_amf_available(
    ffmpeg_bin: str = "ffmpeg",
    encoder: str = "h264_amf",
    *,
    runner: Callable[..., object] | None = None,
) -> None:
    """Probe ``ffmpeg -encoders`` for the requested AMF encoder.

    Raises ``RuntimeError`` if the encoder is not in the FFmpeg
    build (no AMD GPU / driver / FFmpeg without ``--enable-amf``).
    Tests inject a stubbed ``runner`` to exercise both branches.
    """
    runner_fn = runner or subprocess.run
    completed = runner_fn(  # type: ignore[operator]
        [ffmpeg_bin, "-hide_banner", "-encoders"],
        capture_output=True,
        text=True,
        check=False,
    )
    rc = int(getattr(completed, "returncode", 1))
    stdout = getattr(completed, "stdout", "") or ""
    if rc != 0 or encoder not in stdout:
        raise RuntimeError(
            f"AMF encoder {encoder!r} is unavailable in ffmpeg "
            f"({ffmpeg_bin!r}). Confirm an AMD GPU is present and "
            f"ffmpeg was built with --enable-amf."
        )


@dataclasses.dataclass(frozen=True)
class _AMFAdapterBase:
    """Common AMF adapter scaffold — concrete subclasses pin ``encoder``.

    The seven canonical preset names are accepted at the harness
    layer and compressed to AMF's three quality rungs at command
    build time. ``quality_knob`` is ``qp`` (constant-QP),
    ``quality_range`` matches the Phase A x264 window for
    cross-codec comparability, and ``invert_quality`` is True
    because higher QP yields lower visual quality (mirroring CRF).
    """

    name: str = "amf"
    encoder: str = "h264_amf"
    quality_knob: str = "qp"
    # AMF cqp accepts 0..51; surface the Phase A informative window
    # so the search loop's grid generator stays aligned with x264.
    quality_range: tuple[int, int] = (15, 40)
    quality_default: int = 23
    invert_quality: bool = True  # higher qp = lower quality

    # Predictor probe-encode knobs. "ultrafast" maps to AMF "speed".
    probe_preset: str = "ultrafast"
    probe_quality: int = 28
    supports_qpfile: bool = False
    # ADR-0332: AMF is hardware; no first-pass stats-file surface.
    supports_encoder_stats: bool = False
    # ADR-0546: AMF's "two-pass" equivalent is the encoder-internal
    # pre-analysis stage (``-preanalysis true``) which runs inside a
    # single ffmpeg invocation. No standalone first-pass stats sidecar
    # is written; the 2-pass driver therefore falls back to single-
    # pass. Callers that want the pre-analysis quality boost append
    # :meth:`two_pass_args` pass-1 output to ``extra_params``.
    supports_two_pass: bool = False

    presets: tuple[str, ...] = (
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

    def validate(self, preset: str, qp: int) -> None:
        """Raise ``ValueError`` if ``(preset, qp)`` is unsupported."""
        if preset not in self.presets:
            raise ValueError(f"unknown AMF preset {preset!r}; expected one of {self.presets}")
        lo, hi = self.quality_range
        if not lo <= qp <= hi:
            raise ValueError(f"qp {qp} outside Phase A range [{lo}, {hi}] for {self.encoder}")

    def amf_quality(self, preset: str) -> str:
        """Resolve the AMF ``-quality`` argument for a preset name."""
        return map_preset_to_amf_quality(preset)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        """FFmpeg argv slice for AMD AMF single-pass constant-QP.

        AMF does not honour FFmpeg's generic ``-preset``; the rate
        control is constant-QP via ``-rc cqp -qp_i N -qp_p N`` and the
        speed dial is the 3-level ``-quality {speed,balanced,quality}``
        switch. The 7-name canonical preset vocabulary is compressed
        to those three rungs by :func:`map_preset_to_amf_quality`.
        """
        return [
            "-c:v",
            self.encoder,
            "-quality",
            self.amf_quality(preset),
            "-rc",
            "cqp",
            "-qp_i",
            str(quality),
            "-qp_p",
            str(quality),
        ]

    def extra_params(self, preset: str, qp: int) -> tuple[str, ...]:
        """FFmpeg argv tail covering AMF-specific switches.

        Returns the full ``-quality / -rc / -qp_i / -qp_p`` block
        ready to be appended after ``-c:v <encoder>``. The harness
        builds the ``-c:v`` / ``-preset`` portion itself; AMF doesn't
        use the generic ``-preset`` flag, so callers that go through
        ``encode.build_ffmpeg_command`` can pass these via
        ``EncodeRequest.extra_params``.
        """
        return (
            "-quality",
            self.amf_quality(preset),
            "-rc",
            "cqp",
            "-qp_i",
            str(qp),
            "-qp_p",
            str(qp),
        )

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by AMF."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` for AMF.

        AMF honours ``-force_key_frames`` when ``-rc cqp`` is in effect
        (the Phase A rate-control mode this adapter pins via
        ``extra_params``). No equivalent of NVENC's ``-forced-idr`` is
        documented on the AMF side; if downstream decoders trip on
        non-IDR keyframes the workaround is to set ``-bf 0``.
        """
        return _gop_common.default_force_keyframes_args(timestamps)

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """AMD AMF pre-analysis argv (ADR-0546).

        AMF does not implement a software-style two-invocation 2-pass.
        The closest analogue is the encoder-internal pre-analysis
        stage exposed via the ``-preanalysis`` AVOption (``true`` or
        ``false``). Pre-analysis improves rate-distortion decisions
        for VBR rate control and adds modest quality gains for the
        constant-QP mode this adapter pins by default.

        Per-pass contract (mirrors NVENC / QSV):

        - ``pass_number == 0`` → empty tuple (single-pass).
        - ``pass_number == 1`` → ``("-preanalysis", "true")``.
        - ``pass_number == 2`` → empty tuple (pre-analysis already
          ran inside pass-1's single invocation).

        ``stats_path`` is accepted for interface uniformity but unused
        — AMF's pre-analysis state lives in the encoder, not on disk.
        """
        del stats_path  # AMF pre-analysis is in-encoder; no disk sidecar
        if pass_number == 0:
            return ()
        if pass_number == 1:
            return ("-preanalysis", "true")
        if pass_number == 2:
            return ()
        raise ValueError(f"AMF two_pass_args: pass_number must be 0, 1, or 2, got {pass_number}")

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: AMF ``speed`` quality, fixed cqp.

        AMF does not honour FFmpeg's generic ``-preset``; the equivalent
        is the ``-quality {quality,balanced,speed}`` switch threaded
        through ``extra_params``. We inline the speed-mode probe shape
        here rather than calling ``extra_params`` so the probe stays a
        single stable string regardless of any future AMF rate-control
        change.
        """
        return [
            "-c:v",
            self.encoder,
            "-quality",
            self.amf_quality(self.probe_preset),
            "-rc",
            "cqp",
            "-qp_i",
            str(self.probe_quality),
            "-qp_p",
            str(self.probe_quality),
        ]


__all__ = [
    "_AMF_QUALITIES",
    "_AMFAdapterBase",
    "ensure_amf_available",
    "map_preset_to_amf_quality",
]
