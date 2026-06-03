# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Apple VideoToolbox ProRes codec adapter.

FFmpeg encoder name: ``prores_videotoolbox``. Hardware-accelerated
ProRes encoding on Apple Silicon (M1+; ProRes hardware blocks ship on
M1 Pro / Max / Ultra and every later M-series chip). Intel Macs do
**not** have the ProRes hardware block; FFmpeg falls back to the
software ``prores_aw`` / ``prores_ks`` encoders there.

Quality knob
============

ProRes is a **fixed-rate intermediate codec** — it has no CRF / QP /
``-q:v`` style scalar. Quality is selected entirely by the ProRes
**tier** (proxy → lt → 422 → 422 HQ → 4444 → 4444 XQ). Each tier has
an implicit target bitrate that scales with resolution and frame rate;
the encoder itself does not expose a finer dial.

The harness's generic ``crf`` slot carries the integer tier id (0..5)
for ProRes. ``invert_quality=False`` because higher integer = bigger /
more chroma precision = "better" in the harness's monotonic-quality
sense. See ``_videotoolbox_common.PRORES_PROFILE_NAMES`` for the
integer ↔ name lookup.

Preset axis
===========

ProRes VT honours the same ``-realtime`` flag as the H.264 / HEVC VT
encoders (declared via the shared ``COMMON_OPTIONS`` macro in FFmpeg's
``libavcodec/videotoolboxenc.c``), so the adapter reuses the standard
nine-name preset → realtime mapping. The mapping is intentionally
lossy — VT cannot expose a finer dial. For a fixed-rate codec like
ProRes the preset axis mostly affects throughput; tier choice carries
the quality signal.

References
==========

- ADR-0283 (sibling H.264 / HEVC VT adapters, same registry pattern).
- FFmpeg ``libavcodec/videotoolboxenc.c`` ``prores_options`` AVOption
  table (verified 2026-05-09 against an FFmpeg n8.1.1 checkout) for
  the integer profile ids and the named CONST aliases.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path

from . import _gop_common
from ._videotoolbox_common import (
    PRORES_PROFILE_DEFAULT,
    PRORES_PROFILE_RANGE,
    VIDEOTOOLBOX_PRESETS,
    prores_profile_name,
    validate_prores_videotoolbox,
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
class ProresVideoToolboxAdapter:
    """``prores_videotoolbox`` adapter — hardware ProRes on Apple Silicon.

    ProRes is a fixed-rate codec. The ``crf`` slot carries the integer
    tier id (0=proxy → 5=xq); see
    ``_videotoolbox_common.PRORES_PROFILE_NAMES``.
    """

    name: str = "prores_videotoolbox"
    encoder: str = "prores_videotoolbox"
    # Harness-generic "quality knob" name. ProRes uses ``profile:v``
    # rather than ``-q:v`` / ``-crf``, but the corpus row's existing
    # ``crf`` integer column carries the tier id so no schema change
    # is needed. Downstream consumers read ``encoder`` + ``crf``
    # together via the adapter registry to interpret the slot.
    quality_knob: str = "profile:v"
    # Bumps when the adapter's argv shape / preset list / tier window
    # changes (ADR-0298 cache key).
    adapter_version: str = "1"
    quality_range: tuple[int, int] = PRORES_PROFILE_RANGE
    quality_default: int = PRORES_PROFILE_DEFAULT
    invert_quality: bool = False  # higher tier = higher quality

    # Predictor probe-encode knobs. ProRes is fast in hardware; the
    # probe runs at the ``proxy`` tier (smallest output, fastest
    # encode) since the predictor cares about complexity, not quality.
    probe_preset: str = "ultrafast"
    probe_quality: int = 0  # proxy
    supports_qpfile: bool = False
    # ADR-0332: ProRes VideoToolbox does not emit a parseable stats file.
    supports_encoder_stats: bool = False
    # ADR-0546: ProRes is intra-only and VideoToolbox is single-pass
    # by API; 2-pass has no meaning here. :meth:`two_pass_args` raises.
    supports_two_pass: bool = False

    presets: tuple[str, ...] = VIDEOTOOLBOX_PRESETS

    def validate(self, preset: str, crf: int) -> None:
        """Raise ``ValueError`` if ``(preset, profile)`` is unsupported.

        The ``crf`` parameter carries the harness's quality value; for
        ProRes this is the integer tier id on ``[0, 5]``.
        """
        validate_prores_videotoolbox(preset, crf)

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        """FFmpeg argv slice for ``prores_videotoolbox``.

        ``quality`` is the integer tier id (0..5). The argv emits the
        named tier alias (``proxy``, ``lt``, ``standard``, ``hq``,
        ``4444``, ``xq``) rather than the integer — the FFmpeg AVOption
        layer accepts either form, but the named alias is more
        diagnosable in stderr / corpus rows.
        """
        realtime = _PRESET_TO_REALTIME[preset]
        return [
            "-c:v",
            self.encoder,
            "-realtime",
            str(realtime),
            "-profile:v",
            prores_profile_name(quality),
        ]

    def extra_params(self) -> tuple[str, ...]:
        """No additional non-codec argv for VideoToolbox ProRes."""
        return ()

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """ProRes is intra-only; GOP args are no-ops on the codec but
        FFmpeg accepts them silently. We still pin ``-g`` so the
        muxer's seek-table density is predictable.
        """
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """ProRes is intra-only — every frame is a keyframe — so this
        is a no-op-but-harmless override that the harness emits
        uniformly across codecs. Keep the shared default so callers
        do not branch on codec identity.
        """
        return _gop_common.default_force_keyframes_args(timestamps)

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: realtime mode + proxy tier."""
        return _gop_common.default_probe_args(self)

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """Always raise — VideoToolbox is single-pass only (ADR-0546)."""
        return videotoolbox_two_pass_args(self.encoder, pass_number, stats_path)
