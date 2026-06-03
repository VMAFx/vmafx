# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared GOP / keyframe / probe helpers for every codec adapter.

The per-shot predictor PR (see ``docs/adr/`` and ``docs/usage/vmaf-tune-predict.md``)
extends every codec adapter with three new methods:

* ``gop_args(keyint, min_keyint)`` — emits ``-g`` / ``-keyint_min`` (or codec
  equivalent) so the per-shot tuner can extend the GOP inside long low-motion
  shots and shrink it inside high-motion shots.
* ``force_keyframes_args(timestamps)`` — pins keyframes at shot boundaries
  detected by ``vmaf-perShot``. For software codecs FFmpeg's
  ``-force_key_frames`` does the right thing; HW encoders sometimes need a
  codec-specific switch (e.g. NVENC's ``-forced-idr``) on top.
* ``probe_args()`` — returns the FFmpeg argv slice for a fast probe encode
  used by the predictor's complexity estimator. The slice is the same shape
  as ``ffmpeg_codec_args`` so it plugs into the existing dispatcher.

Most software codecs and HW encoders honour FFmpeg's generic ``-g``,
``-keyint_min``, and ``-force_key_frames`` at the FFmpeg layer, so adapters
that need no special handling delegate to ``default_gop_args`` /
``default_force_keyframes_args`` here. Codecs with quirks override.
"""

from __future__ import annotations


def default_gop_args(keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
    """FFmpeg-generic GOP knobs honoured by libx264, libx265, libsvtav1,
    libaom-av1, libvvenc and the NVENC / AMF / QSV families.
    """
    if keyint < 1:
        raise ValueError(f"keyint must be >= 1, got {keyint}")
    args: list[str] = ["-g", str(keyint)]
    if min_keyint is not None:
        if min_keyint < 1 or min_keyint > keyint:
            raise ValueError(f"min_keyint must be in [1, keyint={keyint}], got {min_keyint}")
        args.extend(["-keyint_min", str(min_keyint)])
    return tuple(args)


def default_force_keyframes_args(timestamps: tuple[float, ...]) -> tuple[str, ...]:
    """FFmpeg ``-force_key_frames`` with comma-separated seconds.

    Empty timestamp tuple → no flag emitted (the encoder picks its own
    keyframe cadence). Timestamps are formatted to microsecond precision so
    floating-point boundaries from shot detection round-trip cleanly through
    FFmpeg's parser.
    """
    if not timestamps:
        return ()
    formatted = ",".join(f"{t:.6f}" for t in timestamps)
    return ("-force_key_frames", formatted)


def default_probe_args(adapter: object) -> list[str]:
    """Default probe-encode argv: delegates to
    ``adapter.ffmpeg_codec_args(probe_preset, probe_quality)``.

    Adapters that need a different probe shape (e.g. an encoder whose
    ultrafast preset doesn't expose a usable bitrate signal) override
    this method directly rather than calling here.
    """
    # adapter is duck-typed (CodecAdapter Protocol); attribute + return-type
    # checks deferred to the call site (the dispatcher validates the registry).
    result: list[str] = adapter.ffmpeg_codec_args(  # type: ignore[attr-defined]
        adapter.probe_preset,  # type: ignore[attr-defined]
        adapter.probe_quality,  # type: ignore[attr-defined]
    )
    return result


__all__ = [
    "default_force_keyframes_args",
    "default_gop_args",
    "default_probe_args",
]
