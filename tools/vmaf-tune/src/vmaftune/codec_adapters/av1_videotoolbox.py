# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Apple VideoToolbox AV1 codec adapter (placeholder, self-activating).

FFmpeg encoder name: ``av1_videotoolbox`` — **not yet shipped by FFmpeg
upstream as of n8.1 / master ``8518599cd1`` (2026-05-09)**. The Apple
M3 / M4 silicon family has hardware AV1 encode capability but FFmpeg
has not exposed it. The decoder hwaccel symbol
``ff_av1_videotoolbox_hwaccel`` exists in ``libavcodec``; the encoder
side does not.

This adapter ships in a placeholder state today: registered in the
codec-adapter registry, but ``validate()`` raises
:class:`Av1VideoToolboxUnavailableError` until the runtime probe
detects an FFmpeg build that recognises the encoder. When the probe
succeeds (FFmpeg upstream lands the encoder, fork pulls a recent
master build), :attr:`Av1VideoToolboxAdapter.supports_runtime` flips
to ``True`` and ``validate`` / ``ffmpeg_codec_args`` start succeeding
without any code change here.

The argv shape (``-c:v av1_videotoolbox -realtime 0/1 -q:v <int>``) is
extrapolated from the existing ``h264_videotoolbox`` /
``hevc_videotoolbox`` adapters; per the fork's no-guessing rule, it is
only emitted **after** the runtime probe has confirmed the encoder
exists. If FFmpeg lands the encoder with a different knob shape (e.g.
``-allow_sw`` semantics, a different quality-axis range), the
follow-up PR that flips ``supports_runtime`` to ``True`` will adjust
the argv emission and the placeholder ``quality_range`` accordingly.

See ADR-0339 (placeholder + watcher pattern) and the upstream watcher
at ``scripts/upstream-watcher/check_ffmpeg_av1_videotoolbox.sh``.
"""

from __future__ import annotations

import dataclasses
import shutil
import subprocess
from pathlib import Path

from . import _gop_common
from ._videotoolbox_common import (
    VIDEOTOOLBOX_PRESETS,
    preset_to_realtime,
    videotoolbox_two_pass_args,
)


class Av1VideoToolboxUnavailableError(RuntimeError):
    """The host's FFmpeg build does not expose ``av1_videotoolbox``.

    Raised by :meth:`Av1VideoToolboxAdapter.validate` while the
    placeholder state holds. Callers that catch this can fall back to
    a software AV1 encoder (``libsvtav1``, ``libaom-av1``).
    """


# Sentinel substring FFmpeg prints when an encoder name is unknown:
# ``Codec 'av1_videotoolbox' is not recognized by FFmpeg.``
# Stable across recent FFmpeg releases (matched as a substring, not a
# regex, so a phrasing tweak in a future release degrades gracefully —
# we treat any non-zero exit *or* a missing encoder-options block as
# "unavailable").
_PROBE_NOT_RECOGNIZED_NEEDLE = "is not recognized"

# Substring that appears in the encoder-help output when the encoder
# IS recognized. FFmpeg prints ``Encoder av1_videotoolbox [...]:`` on
# the first line of ``ffmpeg -h encoder=<name>`` for every registered
# encoder.
_PROBE_RECOGNIZED_NEEDLE = "Encoder av1_videotoolbox"


def probe_av1_videotoolbox_available(
    *, ffmpeg_bin: str | None = None, runner=subprocess.run, timeout: float = 5.0
) -> bool:
    """Return ``True`` iff the host's FFmpeg recognises ``av1_videotoolbox``.

    Runs ``ffmpeg -hide_banner -h encoder=av1_videotoolbox`` and
    inspects the merged stdout/stderr for either the not-recognized
    sentinel (returns ``False``) or the encoder-help marker (returns
    ``True``). A missing FFmpeg binary or a non-zero exit other than
    not-recognized also yields ``False`` — the placeholder defaults
    to "inactive" on any uncertainty.

    Parameters
    ----------
    ffmpeg_bin:
        FFmpeg executable path; ``None`` resolves via ``$PATH``.
    runner:
        Injected for tests. Same shape as :func:`subprocess.run`.
    timeout:
        Probe wall-clock cap; FFmpeg's ``-h`` returns near-instantly
        in practice.
    """
    binary = ffmpeg_bin or shutil.which("ffmpeg")
    if binary is None:
        return False
    try:
        completed = runner(
            [binary, "-hide_banner", "-h", "encoder=av1_videotoolbox"],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    blob = (completed.stdout or "") + (completed.stderr or "")
    if _PROBE_NOT_RECOGNIZED_NEEDLE in blob:
        return False
    if _PROBE_RECOGNIZED_NEEDLE in blob:
        return True
    # Defensive: FFmpeg returns 0 + empty output if the encoder name
    # collides with something weird. Treat as unavailable.
    return False


# Placeholder quality window. Inherits the VideoToolbox family
# convention (``-q:v`` on the [0, 100] integer scale, higher = better)
# from h264 / hevc. If the upstream encoder ships with a different
# axis, the activation PR replaces this constant.
_AV1_VT_QUALITY_RANGE: tuple[int, int] = (0, 100)
_AV1_VT_QUALITY_DEFAULT: int = 50


@dataclasses.dataclass(frozen=True)
class Av1VideoToolboxAdapter:
    """``av1_videotoolbox`` adapter — placeholder until upstream lands.

    Stays inactive (``validate`` raises
    :class:`Av1VideoToolboxUnavailableError`) until
    :func:`probe_av1_videotoolbox_available` returns ``True`` for the
    host's FFmpeg build. The probe runs lazily on each ``validate``
    call so a fork sync that pulls a fresh FFmpeg flips behaviour
    automatically — no code edit needed in this adapter.

    Per ADR-0339, the harness reads the protocol-shaped subset of
    fields (``encoder``, ``quality_knob``, etc.) without branching on
    ``name``, so registering this placeholder costs the search loop
    nothing today and gives Apple-AV1 users a stable encoder name
    (``"av1_videotoolbox"``) the moment FFmpeg ships the encoder.
    """

    name: str = "av1_videotoolbox"
    encoder: str = "av1_videotoolbox"
    quality_knob: str = "q:v"
    # Bumps when the placeholder's argv shape changes. The activation
    # PR will bump this so the ADR-0298 cache treats pre-activation
    # rows as stale.
    adapter_version: str = "0-placeholder"
    quality_range: tuple[int, int] = _AV1_VT_QUALITY_RANGE
    quality_default: int = _AV1_VT_QUALITY_DEFAULT
    invert_quality: bool = False  # higher q:v = higher quality

    # Predictor probe-encode knobs. Mirrors the h264/hevc VT defaults;
    # only consumed once the runtime probe activates the adapter.
    probe_preset: str = "ultrafast"
    probe_quality: int = 60
    supports_qpfile: bool = False

    presets: tuple[str, ...] = VIDEOTOOLBOX_PRESETS

    # Static "is this adapter known to the host?" flag. Independent of
    # the runtime probe — it stays ``False`` while the encoder is not
    # in FFmpeg upstream. The activation PR flips this default to
    # ``True`` together with bumping ``adapter_version``.
    supports_runtime: bool = False
    # ADR-0332: VideoToolbox does not emit a parseable x264/x265-style
    # stats file — opt out of encoder-internal stats capture.
    supports_encoder_stats: bool = False
    # ADR-0546: VideoToolbox is single-pass only — see h264_videotoolbox.
    supports_two_pass: bool = False

    def _runtime_available(self) -> bool:
        """Lazily probe ffmpeg; cached only within one ``validate`` call."""
        return probe_av1_videotoolbox_available()

    def validate(self, preset: str, crf: int) -> None:
        """Raise if the host can't run ``av1_videotoolbox`` yet.

        While the placeholder holds, raises
        :class:`Av1VideoToolboxUnavailableError`. Once the runtime
        probe confirms the encoder exists, falls through to the
        standard VideoToolbox preset/quality validation.
        """
        if not self._runtime_available():
            raise Av1VideoToolboxUnavailableError(
                "av1_videotoolbox awaiting upstream FFmpeg encoder support — see ADR-0339"
            )
        if preset not in VIDEOTOOLBOX_PRESETS:
            raise ValueError(
                f"unknown VideoToolbox preset {preset!r}; expected one of {VIDEOTOOLBOX_PRESETS}"
            )
        lo, hi = _AV1_VT_QUALITY_RANGE
        if not lo <= crf <= hi:
            raise ValueError(f"q:v {crf} outside av1_videotoolbox range [{lo}, {hi}]")

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        """FFmpeg argv slice for ``av1_videotoolbox``.

        Refuses to emit argv until the runtime probe has confirmed the
        encoder exists, per the no-guessing rule. The argv shape
        mirrors h264/hevc VT (``-realtime`` boolean + ``-q:v`` int);
        if upstream ships a different shape, the activation PR
        rewrites this method.
        """
        if not self._runtime_available():
            raise Av1VideoToolboxUnavailableError(
                "av1_videotoolbox awaiting upstream FFmpeg encoder support — see ADR-0339"
            )
        realtime = preset_to_realtime(preset)
        return [
            "-c:v",
            self.encoder,
            "-realtime",
            realtime,
            "-q:v",
            str(quality),
        ]

    def extra_params(self) -> tuple[str, ...]:
        """No additional non-codec argv for VideoToolbox."""
        return ()

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by VideoToolbox."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` with comma-separated seconds."""
        return _gop_common.default_force_keyframes_args(timestamps)

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv. Only valid once activated."""
        return _gop_common.default_probe_args(self)

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """Always raise — VideoToolbox is single-pass only (ADR-0546)."""
        return videotoolbox_two_pass_args(self.encoder, pass_number, stats_path)
