# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared helpers for Apple VideoToolbox codec adapters.

VideoToolbox is Apple's hardware encoder API on macOS. FFmpeg exposes
three encoders backed by it: ``h264_videotoolbox``, ``hevc_videotoolbox``,
and ``prores_videotoolbox`` (broadcast / prosumer intermediate). AV1
is not available on Apple Silicon hardware as of 2026 and is
intentionally not surfaced here.

Quality knob
============

Both VT encoders accept ``-q:v`` on the ``[0, 100]`` integer scale
(higher = better quality). This is **different** from x264 / x265's
CRF axis (``[0, 51]``, lower = better quality):

| Codec                  | Knob   | Range     | Direction         |
|------------------------|--------|-----------|-------------------|
| libx264 / libx265      | crf    | 0..51     | lower = better    |
| h264_videotoolbox      | q:v    | 0..100    | higher = better   |
| hevc_videotoolbox      | q:v    | 0..100    | higher = better   |

The corpus row's ``crf`` column carries whatever the harness emitted —
adapters with ``invert_quality=False`` and a different range simply
fill the same slot with their native scale. Downstream consumers read
``encoder`` + ``crf`` together and interpret the knob via the adapter
registry, so the schema stays single-shape.

Preset mapping
==============

VideoToolbox has no x264-style preset axis. The closest knob is the
``-realtime`` flag (boolean): ``-realtime 1`` trades quality for
latency, ``-realtime 0`` (default) targets non-real-time quality.
We map the standard nine-name preset taxonomy onto these two buckets
so callers can drive every codec from one preset list:

  ultrafast / superfast / veryfast / faster / fast → ``-realtime 1``
  medium / slow / slower / veryslow                 → ``-realtime 0``

The mapping is intentionally lossy — VT cannot expose a finer dial.
For per-title encoding the ``q:v`` axis carries the bulk of the
search-space signal; preset mostly affects throughput, not quality.
"""

from __future__ import annotations

from pathlib import Path


class VideoToolboxTwoPassUnsupportedError(NotImplementedError):
    """Apple VideoToolbox does not expose a multi-pass encode API.

    Raised by every ``*_videotoolbox`` adapter's :meth:`two_pass_args`.
    VideoToolbox's ``VTCompressionSession`` (the C API the FFmpeg
    encoders wrap) has no equivalent of x264's ``-pass`` or NVENC's
    ``-multipass``; the encoder runs single-pass only and exposes
    quality solely through ``-q:v`` (h264 / hevc / av1) or ``-bitrate``
    + ``-allow_sw``. Two-pass-style quality lift on an Apple host
    requires switching to a software encoder (libx264, libx265,
    libsvtav1, libaom-av1, libvvenc) — all of which ship in the same
    FFmpeg build.
    """


def videotoolbox_two_pass_args(encoder: str, pass_number: int, stats_path: Path) -> tuple[str, ...]:
    """Always raise :class:`VideoToolboxTwoPassUnsupportedError`.

    Centralises the error message so every ``*_videotoolbox`` adapter
    points users at the same workaround. ``stats_path`` is accepted for
    interface symmetry with the other adapters.
    """
    del stats_path
    raise VideoToolboxTwoPassUnsupportedError(
        f"{encoder!r}: Apple VideoToolbox is single-pass only — the "
        "VTCompressionSession C API does not expose a multi-pass encode "
        "interface (see ADR-0546). pass_number="
        f"{pass_number} is therefore not supported. Switch to a "
        "software encoder (libx264 / libx265 / libsvtav1 / libaom-av1 "
        "/ libvvenc) for true 2-pass on macOS."
    )


# Preset names shared with libx264 / libx265 so callers can use a single
# preset list across codecs. Maps to ``-realtime`` boolean.
_FAST_PRESETS: frozenset[str] = frozenset({"ultrafast", "superfast", "veryfast", "faster", "fast"})
_QUALITY_PRESETS: frozenset[str] = frozenset({"medium", "slow", "slower", "veryslow"})

VIDEOTOOLBOX_PRESETS: tuple[str, ...] = (
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

# VideoToolbox's ``-q:v`` axis. Higher is better; the perceptually
# informative window for both encoders sits in roughly [40, 80] —
# below 40 the output is mostly artefact, above 80 the bitrate
# explodes. The harness still accepts the full hardware range; the
# Phase A grid generator should default to a narrower band.
VIDEOTOOLBOX_QUALITY_RANGE: tuple[int, int] = (0, 100)
VIDEOTOOLBOX_QUALITY_DEFAULT: int = 50


def preset_to_realtime(preset: str) -> str:
    """Translate a preset name to the ``-realtime`` argv value.

    Raises ``ValueError`` for an unknown preset; the adapter's
    ``validate()`` is the upstream gate so this is a defensive check.
    """
    if preset in _FAST_PRESETS:
        return "1"
    if preset in _QUALITY_PRESETS:
        return "0"
    raise ValueError(
        f"unknown VideoToolbox preset {preset!r}; expected one of {VIDEOTOOLBOX_PRESETS}"
    )


def validate_videotoolbox(preset: str, q: int) -> None:
    """Shared validator for the H.264 / HEVC VT adapters.

    The ``q`` argument is the harness's ``crf`` slot; for VT it carries
    the ``-q:v`` value on the ``[0, 100]`` scale.

    ProRes does not use ``-q:v`` and has its own validator
    (``validate_prores_videotoolbox``) — see ``prores_videotoolbox.py``.
    """
    if preset not in VIDEOTOOLBOX_PRESETS:
        raise ValueError(
            f"unknown VideoToolbox preset {preset!r}; expected one of " f"{VIDEOTOOLBOX_PRESETS}"
        )
    lo, hi = VIDEOTOOLBOX_QUALITY_RANGE
    if not lo <= q <= hi:
        raise ValueError(f"q:v {q} outside VideoToolbox range [{lo}, {hi}]")


# -----------------------------------------------------------------------------
# ProRes-specific constants and helpers.
#
# ProRes is a fixed-bitrate intermediate codec — there is no quality
# scalar like CRF or ``-q:v``. Quality is selected by **profile tier**:
# Proxy (lowest, smallest) → LT → 422 (Standard) → 422 HQ → 4444 →
# 4444 XQ (highest, largest). Bitrate is implicit in the tier.
#
# The FFmpeg ``prores_videotoolbox`` encoder exposes the tiers via
# ``-profile:v <int>`` with the integer values defined in
# ``libavcodec/profiles.h`` (``AV_PROFILE_PRORES_PROXY`` … XQ). The
# string aliases ``proxy``, ``lt``, ``standard``, ``hq``, ``4444``,
# ``xq`` map to the same integers via the encoder's named CONST
# AVOptions (see ``libavcodec/videotoolboxenc.c`` ``prores_options``).
#
# The vmaf-tune adapter maps the harness's ``crf`` slot — which is the
# generic "quality knob" the search loop dials — onto the integer tier
# id. The slot is the single source of truth for the chosen tier; the
# corpus-row consumer reads ``encoder`` + ``crf`` together via the
# adapter registry to translate the integer back to a tier name.
# -----------------------------------------------------------------------------

# Integer tier ids as emitted on the FFmpeg argv. Values match
# ``AV_PROFILE_PRORES_*`` in ``libavcodec/profiles.h``.
PRORES_PROFILE_PROXY: int = 0
PRORES_PROFILE_LT: int = 1
PRORES_PROFILE_STANDARD: int = 2  # ProRes 422
PRORES_PROFILE_HQ: int = 3  # ProRes 422 HQ
PRORES_PROFILE_4444: int = 4
PRORES_PROFILE_XQ: int = 5  # ProRes 4444 XQ

# Adapter-visible tier range. Lower = smaller / faster, higher = bigger
# / slower / more chroma precision. ``invert_quality=False`` because
# higher integer = "better" (more bits, more chroma).
PRORES_PROFILE_RANGE: tuple[int, int] = (PRORES_PROFILE_PROXY, PRORES_PROFILE_XQ)

# Default tier for the harness when the caller leaves ``crf`` unset.
# 422 HQ is the most common professional master tier — it is the
# acquisition standard for most non-graphics broadcast workflows.
PRORES_PROFILE_DEFAULT: int = PRORES_PROFILE_HQ

# Tier name lookup, in canonical FFmpeg order. The tuple index is the
# integer tier id (0..5) so callers can do ``PRORES_PROFILE_NAMES[crf]``.
PRORES_PROFILE_NAMES: tuple[str, ...] = (
    "proxy",  # 0
    "lt",  # 1
    "standard",  # 2  (ProRes 422)
    "hq",  # 3  (ProRes 422 HQ)
    "4444",  # 4
    "xq",  # 5  (ProRes 4444 XQ)
)


def prores_profile_name(profile: int) -> str:
    """Return the FFmpeg tier alias for an integer profile id.

    Raises ``ValueError`` for an unknown profile id; the adapter's
    ``validate()`` is the upstream gate so this is a defensive check.
    """
    lo, hi = PRORES_PROFILE_RANGE
    if not lo <= profile <= hi:
        raise ValueError(
            f"unknown ProRes profile {profile!r}; expected an integer in " f"[{lo}, {hi}]"
        )
    return PRORES_PROFILE_NAMES[profile]


def validate_prores_videotoolbox(preset: str, profile: int) -> None:
    """Validator for the ProRes VT adapter.

    The ``profile`` argument is the harness's ``crf`` slot; for ProRes
    it carries the integer tier id on the ``[0, 5]`` scale.
    """
    if preset not in VIDEOTOOLBOX_PRESETS:
        raise ValueError(
            f"unknown VideoToolbox preset {preset!r}; expected one of " f"{VIDEOTOOLBOX_PRESETS}"
        )
    lo, hi = PRORES_PROFILE_RANGE
    if not lo <= profile <= hi:
        raise ValueError(
            f"ProRes profile {profile} outside tier range [{lo}, {hi}] "
            f"(see PRORES_PROFILE_NAMES)"
        )
