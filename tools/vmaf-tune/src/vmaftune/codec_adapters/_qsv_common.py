# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared helpers for Intel QSV codec adapters.

The three QSV encoders (`h264_qsv`, `hevc_qsv`, `av1_qsv`) share the
same preset vocabulary and the same preferred ICQ quality knob
(`global_quality`). This module factors that shared shape out so each
codec adapter is a thin dataclass that pins only what differs (encoder
name, hardware-availability matrix).

QSV preset names map identically to x264-style names (`veryslow`
through `veryfast`) — the FFmpeg QSV bridge accepts the seven names
verbatim. ``preset_to_qsv`` is therefore an identity check that raises
on unknown inputs rather than a translation table.

ICQ rate control (``-global_quality N``) accepts integers in
``[1, 51]`` per the libmfx / VPL spec; values outside that window are
clipped to the encoder's preferred default by the driver, but
``vmaf-tune`` rejects them up-front so corpus rows stay reproducible.

``BaseQsvAdapter`` is a frozen dataclass that supplies the full method
body shared by all three per-codec adapters. Each per-codec class
inherits from it and overrides only the ``name`` / ``encoder`` fields.
"""

from __future__ import annotations

import dataclasses
import itertools
import os
import shutil
import subprocess
import threading
from collections.abc import Callable, Sequence
from pathlib import Path

from ..hw_devices import AUTO_VAAPI_DEVICE, resolve_vaapi_device
from . import _gop_common
from ._ffmpeg_listing import encoder_listed

# QSV preset vocabulary — identical to x264's medium/fast/... subset
# but without the libx264-specific `ultrafast` / `superfast` levels.
QSV_PRESETS: tuple[str, ...] = (
    "veryslow",
    "slower",
    "slow",
    "medium",
    "fast",
    "faster",
    "veryfast",
)

# `global_quality` ICQ window — full libmfx / VPL accepted range.
QSV_QUALITY_RANGE: tuple[int, int] = (1, 51)
QSV_QUALITY_DEFAULT: int = 23

# The 8-bit filter that moves system-memory frames into QSV surfaces;
# qsv_upload_filter selects the depth-aware form (ADR-0601).
QSV_UPLOAD_FILTER: str = "format=nv12,hwupload=extra_hw_frames=64"


@dataclasses.dataclass(frozen=True)
class QsvRateControl:
    """Selected QSV mode and the reason for an explicit CQP fallback."""

    mode: str = "icq"
    reason: str = ""

    @property
    def knob(self) -> str:
        return "-q:v" if self.mode == "cqp-fallback" else "-global_quality"


_QSV_RC_CACHE: dict[tuple[str, str, str], QsvRateControl] = {}
_QSV_RC_LOCK = threading.Lock()


def qsv_rate_control(
    encoder: str,
    *,
    vaapi_device: str = AUTO_VAAPI_DEVICE,
    ffmpeg_bin: str = "ffmpeg",
    runner: Callable[..., object] | None = None,
) -> QsvRateControl:
    """Probe ICQ once per render node, encoder and FFmpeg runtime.

    Reuse the compare probe runner; overrides bypass the cache and probe.
    Serialising cache misses also prevents duplicate concurrent probes.
    """
    override = os.environ.get("VMAFTUNE_QSV_RATE_CONTROL", "auto")
    if override not in ("auto", "icq", "cqp"):
        raise ValueError("VMAFTUNE_QSV_RATE_CONTROL must be auto, icq or cqp")
    if override != "auto":
        return QsvRateControl(
            "icq" if override == "icq" else "cqp-fallback",
            "" if override == "icq" else "VMAFTUNE_QSV_RATE_CONTROL=cqp",
        )
    device = str(Path(resolve_vaapi_device(vaapi_device)).resolve())
    key = (device, encoder, ffmpeg_bin)
    from ..compare import _default_probe_runner, _probe_stdout_text

    run = runner or _default_probe_runner
    with _QSV_RC_LOCK:
        if key not in _QSV_RC_CACHE:
            argv = _qsv_icq_probe_args(ffmpeg_bin, device, encoder)
            try:
                result = run(argv)
                rc = int(getattr(result, "returncode", -1))
                output = _probe_stdout_text(result) or str(getattr(result, "stderr", ""))
                reason = "" if rc == 0 else f"ICQ probe failed (rc={rc}): {output[-1024:]}"
            except (OSError, subprocess.SubprocessError) as exc:
                reason = f"ICQ probe failed: {exc}"
            _QSV_RC_CACHE[key] = QsvRateControl("cqp-fallback" if reason else "icq", reason)
        return _QSV_RC_CACHE[key]


def _qsv_icq_probe_args(ffmpeg_bin: str, device: str, encoder: str) -> list[str]:
    """Build the bounded ICQ smoke encode; no input files or output artifact."""
    argv = [ffmpeg_bin, "-hide_banner", "-loglevel", "error"]
    argv += qsv_device_init_args(device)
    argv += [
        "-f",
        "lavfi",
        "-i",
        "testsrc2=size=320x240:rate=24",
        "-frames:v",
        "8",
        "-vf",
        QSV_UPLOAD_FILTER,
        "-c:v",
        encoder,
        "-preset",
        "veryfast",
        "-global_quality",
        "23",
        "-f",
        "null",
        "-",
    ]
    return argv


def cached_qsv_rate_control(encoder: str, ffmpeg_bin: str = "ffmpeg") -> QsvRateControl | None:
    """Read metadata without starting a probe (compare row rendering)."""
    if not encoder.endswith("_qsv"):
        return None
    override = os.environ.get("VMAFTUNE_QSV_RATE_CONTROL", "auto")
    if override in ("icq", "cqp"):
        return QsvRateControl(
            "icq" if override == "icq" else "cqp-fallback",
            "" if override == "icq" else "VMAFTUNE_QSV_RATE_CONTROL=cqp",
        )
    device = str(Path(resolve_vaapi_device()).resolve())
    with _QSV_RC_LOCK:
        return _QSV_RC_CACHE.get((device, encoder, ffmpeg_bin))


def qsv_upload_filter(pix_fmt: str = "", extra_params: Sequence[str] = ()) -> str:
    """Select NV12 or P010 upload from the target format and output argv.

    The last output ``-pix_fmt`` overrides the request's format. A
    ``-profile:v main10`` request also requires P010, including HDR args.
    Keep this selection identical to Go's ``hwdevice.QSVUploadFilter``.
    """
    profile = ""
    for flag, value in itertools.pairwise(extra_params):
        if flag in ("-pix_fmt", "-pix_fmt:v"):
            pix_fmt = value
        elif flag in ("-profile:v", "-profile"):
            profile = value
    if pix_fmt in ("p010le", "yuv420p10le") or profile == "main10":
        return "format=p010le,hwupload=extra_hw_frames=64"
    return QSV_UPLOAD_FILTER


def qsv_device_init_args(vaapi_device: str = AUTO_VAAPI_DEVICE) -> list[str]:
    """Return the pre-input FFmpeg argv that initialises the QSV device.

    FFmpeg's QSV bridge on Linux needs a VA-API device and a QSV device
    derived from it before the first ``-i``; without them ``-c:v
    h264_qsv`` fails with ``-22 Invalid argument`` even with a working
    Intel driver (ADR-0601). The QSV device is the filter device, so
    :data:`QSV_UPLOAD_FILTER` uploads into frames the encoder takes:
    with the VA-API device there (``-filter_hw_device va``, the chain
    ADR-0601 first recorded) ``hwupload`` produces ``vaapi`` frames and
    the filter graph fails before the encoder opens ("Impossible to
    convert between the formats supported by the filter
    'Parsed_hwupload_1' and the filter 'auto_scale_1'", measured on an
    Arc A380 with the iHD driver, 2026-10-04).
    ``vaapi_device`` resolves through
    :func:`vmaftune.hw_devices.resolve_vaapi_device` (``auto``: the
    ``--vaapi-device`` of the command, ``$VMAFTUNE_VAAPI_DEVICE``, the
    first Intel render node). This is the one implementation of the
    chain: real encodes (:func:`vmaftune.encode.build_ffmpeg_command`)
    and the ``compare`` availability probe both call it.
    """
    resolved = resolve_vaapi_device(vaapi_device)
    return [
        "-init_hw_device",
        f"vaapi=va:{resolved}",
        "-init_hw_device",
        "qsv=qsv_dev@va",
        "-filter_hw_device",
        "qsv_dev",
    ]


def preset_to_qsv(preset: str) -> str:
    """Identity-map a preset name; raise ``ValueError`` if unknown.

    Kept as a function (not a constant lookup) so callers get a single
    error path that matches the other codec adapters.
    """
    if preset not in QSV_PRESETS:
        raise ValueError(f"unknown QSV preset {preset!r}; expected one of {QSV_PRESETS}")
    return preset


def validate_global_quality(value: int) -> None:
    """Raise ``ValueError`` if ``value`` is outside the ICQ window."""
    lo, hi = QSV_QUALITY_RANGE
    if not lo <= value <= hi:
        raise ValueError(f"global_quality {value} outside ICQ range [{lo}, {hi}]")


def ffmpeg_supports_encoder(
    encoder: str,
    *,
    ffmpeg_bin: str = "ffmpeg",
    runner: object | None = None,
) -> bool:
    """Probe whether ``ffmpeg -encoders`` advertises ``encoder``.

    Returns ``False`` when ``ffmpeg_bin`` is missing on ``PATH`` or
    when the encoder line is not present in the listing. ``runner`` is
    parameterised so unit tests can inject a stub without spawning a
    real process — same pattern as ``encode.run_encode`` /
    ``score.run_score``.
    """
    if runner is None and shutil.which(ffmpeg_bin) is None:
        return False
    runner_fn = runner or subprocess.run
    try:
        completed = runner_fn(  # type: ignore[operator]
            [ffmpeg_bin, "-hide_banner", "-encoders"],
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        return False
    stdout = getattr(completed, "stdout", "") or ""
    return encoder_listed(stdout, encoder)


def require_qsv_encoder(
    encoder: str,
    *,
    ffmpeg_bin: str = "ffmpeg",
    runner: object | None = None,
) -> None:
    """Raise ``RuntimeError`` if FFmpeg does not advertise ``encoder``.

    A listing check only. The encode path's availability check is the
    two-stage :func:`vmaftune.compare.probe_encoder_available` (listing
    plus a one-frame encode through :func:`qsv_device_init_args`), which
    the CLI runs before the first encode of a hardware encoder.
    """
    if not ffmpeg_supports_encoder(encoder, ffmpeg_bin=ffmpeg_bin, runner=runner):
        raise RuntimeError(
            f"ffmpeg does not advertise {encoder!r}; rebuild with libmfx / "
            "VPL enabled or use a vendor build that includes Intel QSV"
        )


@dataclasses.dataclass(frozen=True)
class BaseQsvAdapter:
    """Shared implementation for the QSV H.264 / HEVC / AV1 adapters.

    Subclasses override only the two codec-identity fields (``name`` and
    ``encoder``). All method bodies are identical across the three
    per-codec adapters — they live here so the per-codec files each
    collapse to ~15 LOC of field declarations.
    """

    name: str = "h264_qsv"
    encoder: str = "h264_qsv"
    # Cache-key component (ADR-0298): bump when the argv, presets or range
    # change. "3": the upload surface format follows the requested output depth.
    # "4": runtime ICQ probe with named CQP fallback.
    adapter_version: str = "4"
    quality_knob: str = "global_quality"
    quality_range: tuple[int, int] = QSV_QUALITY_RANGE
    quality_default: int = QSV_QUALITY_DEFAULT
    invert_quality: bool = True  # higher global_quality = lower quality

    # Predictor probe-encode knobs. QSV has no "ultrafast"; the QSV
    # preset vocabulary tops out at "veryfast".
    probe_preset: str = "veryfast"
    probe_quality: int = 23
    supports_qpfile: bool = False
    # ADR-0400: hardware encoders have no parseable first-pass stats file.
    supports_encoder_stats: bool = False
    # ADR-0595: QSV's "two-pass" equivalent is the in-encoder
    # extended-BRC look-ahead (``-extbrc 1 -look_ahead_depth 40``)
    # which runs inside a single ffmpeg invocation. There is no
    # standalone first-pass stats sidecar, so the
    # :func:`vmaftune.encode.run_two_pass_encode` driver falls back to
    # a single-pass encode for QSV — callers that want the look-ahead
    # quality boost append :meth:`two_pass_args` pass-1 output to a
    # single-pass ``EncodeRequest.extra_params``.
    supports_two_pass: bool = False
    # ADR-0601: the filter every QSV encode ends its -vf chain with;
    # build_ffmpeg_command appends it to the request's own chain.
    hw_upload_filter: str = QSV_UPLOAD_FILTER

    presets: tuple[str, ...] = QSV_PRESETS

    def hw_upload_filter_for(self, pix_fmt: str, extra_params: Sequence[str] = ()) -> str:
        """Return the upload filter matching this encode's output depth."""
        return qsv_upload_filter(pix_fmt, extra_params)

    def validate(self, preset: str, quality: int) -> None:
        """Raise ``ValueError`` if ``(preset, quality)`` is unsupported."""
        preset_to_qsv(preset)
        validate_global_quality(quality)

    def ffmpeg_codec_args(
        self, preset: str, quality: int, *, rate_control: str = "icq"
    ) -> list[str]:
        """FFmpeg argv slice for ICQ-mode QSV encode.

        ICQ retains ``-global_quality``; an explicit ``cqp-fallback``
        selection uses ``-q:v`` with the same validated quality value.
        Runtime probing belongs to the encode driver, keeping this builder pure.
        """
        self.validate(preset, quality)
        if rate_control not in ("icq", "cqp-fallback"):
            raise ValueError(f"unknown QSV rate control {rate_control!r}")
        return [
            "-c:v",
            self.encoder,
            "-preset",
            preset_to_qsv(preset),
            QsvRateControl(rate_control).knob,
            str(quality),
        ]

    def extra_params(self) -> tuple[str, ...]:
        """No additional non-codec argv for QSV encoders."""
        return ()

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by QSV."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` with comma-separated seconds."""
        return _gop_common.default_force_keyframes_args(timestamps)

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """Intel QSV extended-BRC look-ahead argv (ADR-0595).

        QSV does not implement a software-style two-invocation 2-pass.
        The closest analogue is the extended bit-rate controller's
        look-ahead mode: ``-extbrc 1 -look_ahead_depth 40`` adds an
        internal pre-analysis window inside a single ffmpeg invocation.
        The look-ahead depth of 40 frames matches Intel's recommended
        default for "quality" presets in the libmfx / VPL sample apps.

        Per-pass contract (mirrors the NVENC adapter):

        - ``pass_number == 0`` → empty tuple (single-pass).
        - ``pass_number == 1`` → the full look-ahead flag set.
        - ``pass_number == 2`` → empty tuple (look-ahead already ran
          inside the pass-1 single invocation).

        The adapter declares ``supports_two_pass = False`` so the
        2-pass driver leaves the work to a single invocation; callers
        that want the look-ahead boost can splice this method's pass-1
        return value into ``EncodeRequest.extra_params``.

        ``stats_path`` is accepted for interface uniformity with the
        software adapters but unused — the look-ahead window lives in
        the encoder's working set, not on disk.
        """
        del stats_path  # QSV look-ahead is in-encoder; no disk sidecar
        if pass_number == 0:
            return ()
        if pass_number == 1:
            return ("-extbrc", "1", "-look_ahead_depth", "40")
        if pass_number == 2:
            return ()
        raise ValueError(f"QSV two_pass_args: pass_number must be 0, 1, or 2, got {pass_number}")

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: QSV ``veryfast`` preset, fixed ICQ."""
        return self.ffmpeg_codec_args(self.probe_preset, self.probe_quality)

    def hw_pre_input_args(self, vaapi_device: str = AUTO_VAAPI_DEVICE) -> list[str]:
        """Pre-input device argv of every encode (:func:`qsv_device_init_args`)."""
        return qsv_device_init_args(vaapi_device)

    @staticmethod
    def qsv_hw_init_args(vaapi_device: str = AUTO_VAAPI_DEVICE) -> list[str]:
        """Return the FFmpeg pre-input argv for QSV hardware-device init.

        Same as :func:`qsv_device_init_args`; kept for callers of the
        static helper (ADR-0601, Bug V14-B). Encodes get the chain and
        the :data:`QSV_UPLOAD_FILTER` from
        :func:`vmaftune.encode.build_ffmpeg_command` without calling it.
        """
        return qsv_device_init_args(vaapi_device)
