# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""SVT-AV1 (``libsvtav1``) codec adapter — Phase A companion.

Mirrors the :class:`X264Adapter` interface for the AV1 reference encoder
(Intel / Netflix / Meta SVT-AV1). Drives the encoder via FFmpeg's
``-c:v libsvtav1`` so the corpus harness keeps a single subprocess seam.

Key differences from x264 (encoded as data, not branches):

* CRF range is 0..63 (x264 / x265 use 0..51). The Phase A informative
  window is pinned to ``(20, 50)`` — VMAF responds monotonically across
  that window for typical OTT bitrates.
* Presets are integers 0..13 (0 = slowest / best, 13 = fastest). To stay
  compatible with the x264-style preset *names* the harness already
  accepts on the command line, this adapter keeps a deterministic
  name -> int mapping (see :data:`PRESET_NAME_TO_INT`).
* The default preset (``"medium"``) maps to integer ``7`` — the upstream
  SVT-AV1 default and the most common reference point in published
  AV1-vs-x264 quality plots.

The adapter is intentionally a thin policy object: validation and the
name-to-int lookup are pure functions. The actual FFmpeg command is
shared with x264 via :func:`vmaftune.encode.build_ffmpeg_command`; the
preset value emitted into argv is the *integer string* (e.g. ``"7"``)
because FFmpeg's libsvtav1 wrapper accepts integer presets through
``-preset``.
"""

from __future__ import annotations

import dataclasses
from collections.abc import Mapping
from pathlib import Path

from . import _gop_common

# x264-style preset names mapped onto SVT-AV1's 0..13 integer scale.
# Chosen to span the SVT-AV1 range with the same "slowest..fastest"
# semantics x264 users already have in their muscle memory:
#
#   placebo  -> 0   (slowest, research-grade only)
#   slowest  -> 1
#   slower   -> 3
#   slow     -> 5
#   medium   -> 7   (SVT-AV1 default)
#   fast     -> 9
#   faster   -> 11
#   veryfast -> 13  (fastest)
#
# The integers are the values FFmpeg's libsvtav1 wrapper forwards to
# SvtAv1EncApp's ``--preset`` argument unchanged.
PRESET_NAME_TO_INT: Mapping[str, int] = {
    "placebo": 0,
    "slowest": 1,
    "slower": 3,
    "slow": 5,
    "medium": 7,
    "fast": 9,
    "faster": 11,
    "veryfast": 13,
}


def preset_to_int(preset: str) -> int:
    """Translate an x264-style preset *name* to the SVT-AV1 integer.

    Raises :class:`ValueError` if ``preset`` is not in
    :data:`PRESET_NAME_TO_INT`.
    """
    if preset not in PRESET_NAME_TO_INT:
        raise ValueError(
            f"unknown svtav1 preset {preset!r}; expected one of " f"{tuple(PRESET_NAME_TO_INT)}"
        )
    return PRESET_NAME_TO_INT[preset]


@dataclasses.dataclass(frozen=True)
class SvtAv1Adapter:
    """``libsvtav1`` single-pass CRF adapter.

    Parameter semantics match :class:`X264Adapter` so the corpus loop
    can stay codec-agnostic. AV1-specific shape lives in the field
    defaults below — the adapter is configuration, not behaviour.
    """

    name: str = "libsvtav1"
    encoder: str = "libsvtav1"
    quality_knob: str = "crf"
    # SVT-AV1 nominally accepts CRF 0..63. Phase A surfaces the
    # perceptually informative window — ADR-0277 covers the choice.
    quality_range: tuple[int, int] = (20, 50)
    quality_default: int = 35
    invert_quality: bool = True  # higher CRF = lower quality

    # Predictor probe-encode knobs. svtav1 maps "veryfast" to integer 13
    # (the fastest preset) via PRESET_NAME_TO_INT.
    probe_preset: str = "veryfast"
    probe_quality: int = 35
    supports_qpfile: bool = False
    # ADR-0332: this encoder has no parseable first-pass stats file.
    supports_encoder_stats: bool = False
    # SVT-AV1 QP-offset map (ADR-0370): delivered via --qp-file / -svtav1-params.
    supports_saliency_roi: bool = True
    # ADR-0546 (Phase F real 2-pass): SVT-AV1 supports multi-pass
    # **only in VBR mode**. Quoting the encoder's runtime error
    # (verified against SVT-AV1 v4.1.0): "CRF does not support
    # multi-pass. Use single pass." vmaf-tune pins libsvtav1 to CRF
    # mode (the harness's canonical quality axis), so the 2-pass
    # driver leaves ``supports_two_pass = False`` and
    # :meth:`two_pass_args` returns the VBR-mode argv plus emits a
    # diagnostic. Callers who explicitly switch the adapter to a
    # bitrate-targeted mode can use those args.
    supports_two_pass: bool = False

    # Phase-A-supported preset *names* (compatibility shim — see
    # PRESET_NAME_TO_INT). Order is "slowest -> fastest" to match the
    # x264 presets tuple.
    presets: tuple[str, ...] = (
        "placebo",
        "slowest",
        "slower",
        "slow",
        "medium",
        "fast",
        "faster",
        "veryfast",
    )

    # Hard limits per the SVT-AV1 spec, surfaced for callers that need
    # to reason about the full encoder range (e.g. CI integration).
    crf_min: int = 0
    crf_max: int = 63
    preset_min: int = 0
    preset_max: int = 13

    def validate(self, preset: str, crf: int) -> None:
        """Raise :class:`ValueError` if ``(preset, crf)`` is unsupported.

        Two layers of validation:

        1. ``crf`` must be in the absolute SVT-AV1 range ``[0, 63]`` —
           anything outside is an encoder-level invalid argument and
           the harness must reject it before spawning FFmpeg.
        2. ``crf`` must lie inside the Phase A informative window. The
           absolute-range check fires first so users get a clear
           "CRF > 63 is invalid for AV1" rather than a Phase-A
           message that suggests it might still be a legal value.
        """
        if preset not in self.presets:
            raise ValueError(
                f"unknown svtav1 preset {preset!r}; expected one of " f"{self.presets}"
            )
        if not self.crf_min <= crf <= self.crf_max:
            raise ValueError(
                f"crf {crf} outside SVT-AV1 absolute range " f"[{self.crf_min}, {self.crf_max}]"
            )
        lo, hi = self.quality_range
        if not lo <= crf <= hi:
            raise ValueError(f"crf {crf} outside Phase A range [{lo}, {hi}]")

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        """FFmpeg argv slice for libsvtav1 single-pass CRF.

        SVT-AV1's FFmpeg wrapper accepts integer presets through the
        generic ``-preset`` flag; the integer is the
        :data:`PRESET_NAME_TO_INT` lookup of the canonical preset name.
        """
        return [
            "-c:v",
            self.encoder,
            "-preset",
            self.ffmpeg_preset_token(preset),
            "-crf",
            str(quality),
        ]

    def extra_params(self) -> tuple[str, ...]:
        """No additional non-codec argv for libsvtav1 Phase A."""
        return ()

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by libsvtav1."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` with comma-separated seconds."""
        return _gop_common.default_force_keyframes_args(timestamps)

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: fastest preset (integer), fixed CRF."""
        return [
            "-c:v",
            self.encoder,
            "-preset",
            self.ffmpeg_preset_token(self.probe_preset),
            "-crf",
            str(self.probe_quality),
        ]

    def ffmpeg_preset_token(self, preset: str) -> str:
        """Return the string the FFmpeg ``-preset`` argv slot expects.

        SVT-AV1 takes integer presets, so the corpus harness translates
        the x264-style name to its integer equivalent before invoking
        FFmpeg. Returned as a string because that is what the argv list
        already carries elsewhere.
        """
        return str(preset_to_int(preset))

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """FFmpeg argv slice for libsvtav1 2-pass encoding (ADR-0546).

        SVT-AV1 supports multi-pass **only in VBR / target-bitrate
        mode**. The encoder enforces this at runtime: pass 2 of a CRF
        2-pass fails with::

            Svt[error]: CRF does not support multi-pass. Use single pass.

        vmaf-tune's libsvtav1 adapter pins CRF mode by default (CRF is
        the harness's canonical quality axis), so the
        :func:`vmaftune.encode.run_two_pass_encode` driver leaves
        ``supports_two_pass = False`` and falls back to single-pass.

        This method still returns the VBR-mode 2-pass argv for callers
        that have explicitly switched the encoder to bitrate-targeted
        mode (e.g. via ``EncodeRequest.extra_params``). The FFmpeg
        ``libsvtav1`` wrapper accepts the generic ``-pass`` /
        ``-passlogfile`` flags; the stats sidecar is
        ``<passlogfile>-0.log`` as with libx264 / libaom-av1.

        Returns an empty tuple for ``pass_number == 0`` so callers that
        forward this method's result unconditionally don't need a
        single-pass branch.
        """
        if pass_number == 0:
            return ()
        if pass_number not in (1, 2):
            raise ValueError(
                f"libsvtav1 two_pass_args: pass_number must be 1 or 2, got {pass_number}"
            )
        return ("-pass", str(pass_number), "-passlogfile", str(stats_path))

    def qpmap_from_saliency(
        self,
        block_offsets: object,
        out_path: object,
        *,
        duration_frames: int = 1,
    ) -> object:
        """Write an SVT-AV1 QP-offset map file from a per-SB-block offset array.

        Delegates to :func:`vmaftune.saliency.write_svtav1_qpoffset_map`.
        Returns the output ``Path`` the caller passes to
        :func:`vmaftune.saliency.augment_extra_params_with_svtav1_qpmap`.

        ``block_offsets`` must be at 64x64 super-block granularity —
        reduce via :func:`vmaftune.saliency.reduce_qp_map_to_blocks`
        with ``block=SVTAV1_SB_SIDE`` (ADR-0370).
        """
        from pathlib import Path as _Path

        from vmaftune.saliency import write_svtav1_qpoffset_map  # local import

        return write_svtav1_qpoffset_map(
            block_offsets, _Path(out_path), duration_frames=duration_frames
        )
