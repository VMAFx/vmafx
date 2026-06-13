# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Codec adapter registry.

Per ADR-0237, every codec exposes a different parameter shape; the
harness must not branch on codec identity in the search loop. Each
adapter declares its quality knob, range, defaults, and FFmpeg encoder
name. Phase A wires ``libx264`` plus the NVIDIA NVENC family
(``h264_nvenc``, ``hevc_nvenc``, ``av1_nvenc``), the AMD AMF family
(``h264_amf``, ``hevc_amf``, ``av1_amf``), the Intel QSV family
(``h264_qsv``, ``hevc_qsv``, ``av1_qsv``), the Apple VideoToolbox
family (``h264_videotoolbox``, ``hevc_videotoolbox``,
``prores_videotoolbox``), the Fraunhofer VVenC H.266 encoder
(``libvvenc``), the SVT-AV1 software encoder (``libsvtav1``), and
the VP9 software encoder (``libvpx-vp9``) —
software and hardware encoders share the same adapter contract;
later phases add one file per codec without touching the search loop.

Mnemonic preset names (``ultrafast``..``placebo``) are normalised
across software and hardware encoders. NVENC's seven hardware presets
(``p1``..``p7``) collapse the ten mnemonic names per the table in
``_nvenc_common``: ``ultrafast``/``superfast``/``veryfast`` → ``p1``,
``faster`` → ``p2``, ``fast`` → ``p3``, ``medium`` → ``p4``,
``slow`` → ``p5``, ``slower`` → ``p6``, ``slowest``/``placebo`` →
``p7``.
"""

from __future__ import annotations

from pathlib import Path
from typing import Protocol

from ._nvenc_common import BaseNvencAdapter
from ._qsv_common import BaseQsvAdapter
from .av1_amf import AV1AMFAdapter
from .av1_nvenc import Av1NvencAdapter
from .av1_qsv import Av1QsvAdapter
from .av1_videotoolbox import Av1VideoToolboxAdapter, Av1VideoToolboxUnavailableError
from .h264_amf import H264AMFAdapter
from .h264_nvenc import H264NvencAdapter
from .h264_qsv import H264QsvAdapter
from .h264_videotoolbox import H264VideoToolboxAdapter
from .hevc_amf import HEVCAMFAdapter
from .hevc_nvenc import HevcNvencAdapter
from .hevc_qsv import HevcQsvAdapter
from .hevc_videotoolbox import HEVCVideoToolboxAdapter
from .libaom import LibaomAdapter
from .libvpx import LibvpxVp9Adapter
from .prores_videotoolbox import ProresVideoToolboxAdapter
from .svtav1 import SvtAv1Adapter
from .vvenc import VVenCAdapter
from .x264 import X264Adapter
from .x265 import X265Adapter


class CodecAdapter(Protocol):
    """Codec-adapter contract (ADR-0237 Phase A + ADR-0294 dispatcher).

    The encode dispatcher (``encode.run_encode``) consumes the
    runtime-shaped subset (``encoder``, ``ffmpeg_codec_args``,
    ``extra_params``) and never branches on the adapter's ``name`` —
    that's the invariant that lets new codec adapters drop in
    one-PR-at-a-time without touching the search loop.
    """

    name: str
    encoder: str
    quality_knob: str
    quality_range: tuple[int, int]
    quality_default: int
    invert_quality: bool
    # The encoder's named preset tuple — e.g. ``("ultrafast", "fast",
    # "medium", "slow", "veryslow")`` for x264 — surfaced through the
    # ladder / sampler default-preset picker (see ``ladder.py``).
    # Every concrete adapter declares this; promoting it to the Protocol
    # so callers like ``_default_sampler_preset`` typecheck without
    # ``getattr`` indirection.
    presets: tuple[str, ...]
    # Bumps when the adapter's argv shape / preset list / quality
    # window changes — see ADR-0298 (vmaf-tune cache key).
    adapter_version: str

    # Predictor probe-encode knobs (see _gop_common docstring).
    # The per-shot predictor consumes these to run one fast probe encode
    # per shot and read its bitrate as the complexity barometer.
    probe_preset: str
    probe_quality: int
    supports_qpfile: bool
    # ADR-0332: opt-in to the pass-1 stats-file capture path. True
    # iff the encoder writes a parseable per-frame stats file under
    # ``-pass 1 -passlogfile <prefix>``. Software encoders that
    # share x264-family rate-distortion tracking (libx264, libx265)
    # set True; hardware encoders (NVENC / AMF / QSV / VideoToolbox)
    # and any encoder without a text stats-file surface set False.
    # libvpx-vp9 uses a binary packet layout (``vpx_codec_pkt_t``
    # / ``VPX_CODEC_STATS_PKT``) and sets False until a binary
    # packet parser is contributed. The ``encoder_stats`` module
    # normalises both x264 and x265 text formats via their field
    # aliases (``q-aq``, ``icu``, ``pcu``, ``scu``).
    supports_encoder_stats: bool

    # Phase F (ADR-0333). Adapters that opt into 2-pass encoding set
    # ``supports_two_pass = True`` AND override
    # :meth:`two_pass_args`. The default (``False`` + an empty
    # tuple) keeps single-pass adapters single-pass; the encode
    # driver detects the flag and falls back gracefully when
    # ``--two-pass`` is requested against a non-supporting codec.
    supports_two_pass: bool

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        """FFmpeg ``-c:v ...`` argv slice for one encode."""
        ...

    def extra_params(self) -> tuple[str, ...]:
        """Additional non-codec argv (e.g. tile flags). May be empty."""
        ...

    def validate(self, preset: str, quality: int) -> None:
        """Raise ``ValueError`` if ``(preset, quality)`` is unsupported."""
        ...

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg argv slice that pins the GOP / keyint. Empty tuple = leave default."""
        ...

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg argv slice that pins keyframes at the given seconds. Empty tuple = no override."""
        ...

    def probe_args(self) -> list[str]:
        """FFmpeg argv slice for a fast probe encode (predictor complexity barometer)."""
        ...

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """FFmpeg argv slice for the Nth pass of a 2-pass encode (Phase F).

        ``pass_number`` is 1 (first pass — analyse) or 2 (second pass —
        encode using the stats from pass 1). ``pass_number == 0`` is
        treated as single-pass and returns an empty tuple. Adapters
        with ``supports_two_pass = False`` raise
        :class:`NotImplementedError`; the encode driver checks the
        flag before calling this method.
        """
        ...


_REGISTRY: dict[str, CodecAdapter] = {
    "libx264": X264Adapter(),
    "libaom-av1": LibaomAdapter(),
    "libx265": X265Adapter(),
    "h264_nvenc": H264NvencAdapter(),
    "hevc_nvenc": HevcNvencAdapter(),
    "av1_nvenc": Av1NvencAdapter(),
    "h264_amf": H264AMFAdapter(),
    "hevc_amf": HEVCAMFAdapter(),
    "av1_amf": AV1AMFAdapter(),
    "h264_qsv": H264QsvAdapter(),
    "hevc_qsv": HevcQsvAdapter(),
    "av1_qsv": Av1QsvAdapter(),
    "h264_videotoolbox": H264VideoToolboxAdapter(),
    "hevc_videotoolbox": HEVCVideoToolboxAdapter(),
    "prores_videotoolbox": ProresVideoToolboxAdapter(),
    "av1_videotoolbox": Av1VideoToolboxAdapter(),
    "libvvenc": VVenCAdapter(),
    "libsvtav1": SvtAv1Adapter(),
    "libvpx-vp9": LibvpxVp9Adapter(),
}


def get_adapter(name: str) -> CodecAdapter:
    if name not in _REGISTRY:
        raise KeyError(f"unknown codec {name!r}; known codecs: {sorted(_REGISTRY)}")
    return _REGISTRY[name]


def known_codecs() -> tuple[str, ...]:
    return tuple(sorted(_REGISTRY))


def parse_available_codecs(
    ffmpeg_encoders_stdout: str,
    *,
    restrict_to_known: bool = True,
) -> frozenset[str]:
    """Parse the output of ``ffmpeg -hide_banner -encoders`` into a frozenset.

    ADR-0498 follow-up #7: this is the codec-list parser that was deferred
    from the initial ``codec_adapters`` scaffolding (line 97: "codec-list
    parser arrives in a follow-up").  The parser turns the encoder table
    into a set of available codec names so callers can gate
    ``supports_encoder_stats`` capture and other codec-specific paths on
    runtime availability rather than compile-time assumptions.

    Each non-header line in ``ffmpeg -encoders`` output has the form::

        " V..... libx264          H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10"

    where the first column is a capability-flags string (``V`` for video,
    ``.`` for unset flags) and the second is the encoder name.  Header
    lines (``"Encoders:"``, ``" ------"``, blank) are skipped.

    Parameters
    ----------
    ffmpeg_encoders_stdout:
        Raw stdout from ``ffmpeg -hide_banner -encoders``.
    restrict_to_known:
        When ``True`` (the default) only names that appear in the
        codec-adapter registry (:func:`known_codecs`) are returned.
        Set to ``False`` to get the full set reported by ffmpeg.

    Returns
    -------
    frozenset[str]
        Codec names available in this ffmpeg build.

    Examples
    --------
    >>> out = subprocess.check_output(["ffmpeg", "-hide_banner", "-encoders"], text=True)
    >>> available = parse_available_codecs(out)
    >>> "libx264" in available
    True
    """
    found: set[str] = set()
    for raw in ffmpeg_encoders_stdout.splitlines():
        line = raw.strip()
        if not line or "------" in line or line.startswith("Encoders"):
            continue
        tokens = line.split()
        # Encoder lines: first token is capability flags (e.g. ``V.....``),
        # second is the encoder name.  Skip non-encoder header lines.
        if len(tokens) < 2:
            continue
        flags, name = tokens[0], tokens[1]
        if len(flags) >= 1 and flags[0] in ("V", "A", "S"):
            found.add(name)
    if restrict_to_known:
        known = set(known_codecs())
        return frozenset(found & known)
    return frozenset(found)


__all__ = [
    "AV1AMFAdapter",
    "Av1NvencAdapter",
    "BaseNvencAdapter",
    "BaseQsvAdapter",
    "Av1QsvAdapter",
    "Av1VideoToolboxAdapter",
    "Av1VideoToolboxUnavailableError",
    "CodecAdapter",
    "H264AMFAdapter",
    "H264NvencAdapter",
    "H264QsvAdapter",
    "H264VideoToolboxAdapter",
    "HEVCAMFAdapter",
    "HEVCVideoToolboxAdapter",
    "HevcNvencAdapter",
    "HevcQsvAdapter",
    "LibaomAdapter",
    "LibvpxVp9Adapter",
    "ProresVideoToolboxAdapter",
    "SvtAv1Adapter",
    "VVenCAdapter",
    "X264Adapter",
    "X265Adapter",
    "get_adapter",
    "known_codecs",
    "parse_available_codecs",
]
