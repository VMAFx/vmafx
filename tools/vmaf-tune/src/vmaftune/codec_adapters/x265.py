# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""libx265 codec adapter — ADR-0288 + ADR-0333.

Mirrors :mod:`vmaftune.codec_adapters.x264` shape; differs only in the
codec-specific defaults (ten preset levels including ``placebo``,
default profile derived from pixel format). Phase A still drives the
ffmpeg subprocess via :mod:`vmaftune.encode`; this adapter contributes
metadata + per-codec validation only.

Phase F (ADR-0333) adds 2-pass support via the
:meth:`X265Adapter.two_pass_args` method. libx265's 2-pass switches
flow through ``-x265-params pass=N:stats=<path>`` rather than the
standalone ``-pass``/``-passlogfile`` ffmpeg flags x264 uses.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path

from . import _gop_common

# Pixel-format → x265 profile mapping. Keys cover the common YUV
# fixture shapes the harness ingests; unmapped formats fall back to
# ``main`` (8-bit) to keep the adapter forgiving.
_PROFILE_BY_PIX_FMT: dict[str, str] = {
    "yuv420p": "main",
    "yuv422p": "main422-8",
    "yuv444p": "main444-8",
    "yuv420p10le": "main10",
    "yuv422p10le": "main422-10",
    "yuv444p10le": "main444-10",
    "yuv420p12le": "main12",
}


@dataclasses.dataclass(frozen=True)
class X265Adapter:
    """libx265 single-pass CRF adapter.

    Defaults match the ADR-0237 Phase A grid window for x264; the
    perceptually-informative CRF range is identical (HEVC's CRF axis
    is linear-perceptual on the same 0..51 scale).
    """

    name: str = "libx265"
    encoder: str = "libx265"
    quality_knob: str = "crf"
    # x265 nominally accepts 0..51; surface the same Phase A informative
    # window as x264 so the search loop is uniform across codecs.
    quality_range: tuple[int, int] = (15, 40)
    quality_default: int = 28  # x265 default; ~visually-lossless on most content
    invert_quality: bool = True  # higher CRF = lower quality

    # Predictor probe-encode knobs.
    probe_preset: str = "ultrafast"
    probe_quality: int = 28
    # libx265 saliency ROI is delivered via the --zones argv format
    # (ADR-0370). ``supports_qpfile`` tracks x264-compatible qpfile
    # support; x265 uses its own zones channel and does not share the
    # x264 ASCII qpfile format, so this remains False.
    supports_qpfile: bool = False
    # Zones-based saliency ROI is available for x265 (ADR-0370).
    supports_saliency_roi: bool = True
    # ADR-0332: libx265 emits a pass-1 text stats file whose q-aq and
    # CTU-count aliases are normalised by ``encoder_stats``.
    supports_encoder_stats: bool = True

    # Phase F (ADR-0333): libx265 supports 2-pass encoding via
    # ``-x265-params pass=N:stats=<path>``. The harness opts in via
    # ``EncodeRequest.pass_number`` + ``--two-pass`` CLI flag.
    supports_two_pass: bool = True

    # x265 ships ten presets — one more than x264 (adds ``placebo``).
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
        "placebo",
    )

    def validate(self, preset: str, crf: int) -> None:
        """Raise ``ValueError`` if ``(preset, crf)`` is unsupported."""
        if preset not in self.presets:
            raise ValueError(f"unknown x265 preset {preset!r}; expected one of {self.presets}")
        lo, hi = self.quality_range
        if not lo <= crf <= hi:
            raise ValueError(f"crf {crf} outside Phase A range [{lo}, {hi}]")

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        """FFmpeg argv slice for libx265 single-pass CRF.

        Byte-for-byte identical to the legacy hardcode in
        ``encode.build_ffmpeg_command`` so HP-1's dispatcher pivot does
        not change x265 argv composition. See ADR-0288 / ADR-0326.
        """
        return ["-c:v", self.encoder, "-preset", preset, "-crf", str(quality)]

    def extra_params(self) -> tuple[str, ...]:
        """No additional non-codec argv for libx265 Phase A."""
        return ()

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by libx265 verbatim."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` with comma-separated seconds."""
        return _gop_common.default_force_keyframes_args(timestamps)

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: ultrafast preset, fixed CRF."""
        return ["-c:v", self.encoder, "-preset", self.probe_preset, "-crf", str(self.probe_quality)]

    def profile_for(self, pix_fmt: str) -> str:
        """Return the canonical x265 profile string for ``pix_fmt``.

        Falls back to ``main`` (8-bit 4:2:0) for unknown pixel formats —
        callers that need a stricter behaviour should consult
        :data:`_PROFILE_BY_PIX_FMT` directly.
        """
        return _PROFILE_BY_PIX_FMT.get(pix_fmt, "main")

    def zones_from_saliency(
        self,
        block_offsets: object,
        *,
        duration_frames: int = 1,
    ) -> str:
        """Produce the x265 ``--zones`` string from a per-block QP-offset array.

        Delegates to :func:`vmaftune.saliency.write_x265_zones_arg`.
        Returns the raw string value the caller appends via
        :func:`vmaftune.saliency.augment_extra_params_with_x265_zones`.

        ``block_offsets`` is an ``int32 [bh, bw]`` array at 16x16
        macroblock granularity (the output of
        :func:`vmaftune.saliency.reduce_qp_map_to_blocks` with
        ``block=X264_MB_SIDE``).
        """
        from vmaftune.saliency import write_x265_zones_arg  # local import

        return write_x265_zones_arg(block_offsets, duration_frames=duration_frames)

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """FFmpeg argv slice for libx265 2-pass encoding (Phase F / ADR-0333).

        libx265 routes pass control through ``-x265-params``, not the
        standalone ``-pass``/``-passlogfile`` ffmpeg flags x264 uses.
        The argv shape is::

            -x265-params pass=<N>:stats=<path>

        where ``N`` is 1 (first pass — analyse, write stats) or 2
        (second pass — read stats, encode). A 3-pass refinement is
        possible on libx265 but not exposed by this adapter.

        Raises :class:`ValueError` for ``pass_number`` outside ``{1, 2}``.
        Returns an empty tuple when ``pass_number == 0`` so callers that
        forward this method's result unconditionally don't need a
        single-pass branch.
        """
        if pass_number == 0:
            return ()
        if pass_number not in (1, 2):
            raise ValueError(
                f"libx265 two_pass_args: pass_number must be 1 or 2, got {pass_number}"
            )
        # ``stats_path`` may carry colons on some platforms (Windows
        # drive letters); x265 parses ``-x265-params`` as colon-
        # separated key=value pairs, so a colon in the path would split
        # the directive. The encode driver materialises the stats file
        # in a tempdir under ``tempfile.gettempdir()`` (POSIX paths on
        # the supported runners); a future Windows port that lands a
        # path with a colon would need to translate it (e.g. via the
        # short-name) before reaching this method.
        return ("-x265-params", f"pass={pass_number}:stats={stats_path}")
