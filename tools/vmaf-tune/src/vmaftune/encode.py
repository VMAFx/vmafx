# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""ffmpeg/libx264 driver — Phase A.

Wraps a single ffmpeg invocation that re-encodes a raw YUV source with
``libx264`` at a given (preset, crf). Captures wall time, output size,
and the encoder's reported version string.

Subprocess boundary is the integration seam — tests mock
``subprocess.run`` rather than running ffmpeg.

Phase F (ADR-0333) adds 2-pass encoding via :func:`run_two_pass_encode`
and the ``pass_number`` / ``stats_path`` fields on :class:`EncodeRequest`.
The single-pass path (``pass_number == 0``, the default) is unchanged.
"""

from __future__ import annotations

import dataclasses
import os
import re
import subprocess
import sys
import tempfile
import time
import uuid
from collections.abc import Sequence
from pathlib import Path

from .encoder_stats import PerFrameStats, parse_stats_file


@dataclasses.dataclass(frozen=True)
class EncodeRequest:
    """Single (preset, crf) request against one raw YUV source.

    ``sample_clip_seconds`` opts the request into sample-clip mode
    (ADR-0297): FFmpeg input is sliced to the centre N-second window of
    the reference, cutting encode time roughly linearly with the slice
    length. ``0.0`` (default) keeps the legacy full-source encode.
    ``sample_clip_start_s`` is the start offset (set by the caller from
    ``duration_s`` and ``sample_clip_seconds``); the encode driver does
    not recompute it so that the score driver can mirror the same
    window via ``--frame_skip_ref`` / ``--frame_cnt``.
    """

    source: Path
    width: int
    height: int
    pix_fmt: str
    framerate: float
    encoder: str
    preset: str
    crf: int
    output: Path
    extra_params: tuple[str, ...] = ()
    sample_clip_seconds: float = 0.0
    sample_clip_start_s: float = 0.0
    # Phase F (ADR-0333): 2-pass encoding control. ``pass_number`` is
    # 0 (single-pass / default), 1 (first pass — analyse, write
    # stats), or 2 (second pass — read stats, encode). ``stats_path``
    # is the per-encode unique stats file path; required when
    # ``pass_number != 0``. The driver materialises the stats file
    # itself in :func:`run_two_pass_encode`; callers building one
    # ``EncodeRequest`` at a time can leave ``stats_path = None`` for
    # single-pass.
    pass_number: int = 0
    stats_path: Path | None = None
    # When True, the source is a container file (mkv/mp4/…) not raw YUV.
    # build_ffmpeg_command omits the -f rawvideo / -pix_fmt / -s / -r
    # input flags so ffmpeg auto-detects the format. sample_clip_seconds
    # uses -ss/-t on the input side when set.
    source_is_container: bool = False
    # BBB e2e v6 Bug #V6-1 (ADR-0506): when the corpus job is bound to
    # an analysed-window length (``CorpusJob.duration_s > 0``) and the
    # caller has NOT opted into sample-clip mode, the encode must still
    # be clipped to that window — otherwise a 30 fps source 9 min long
    # bound to ``duration_s=10`` would encode all 9 min while only 10 s
    # of reference is decoded for scoring. ``duration_s`` here is a
    # plumb-through of ``CorpusJob.duration_s``; the encode driver
    # adds ``-t duration_s`` on the input side when
    # ``sample_clip_seconds == 0.0 and duration_s > 0`` so the
    # `--duration` ladder/CLI flag actually bounds the encode.
    duration_s: float = 0.0


@dataclasses.dataclass(frozen=True)
class EncodeResult:
    """Outcome of one encode call.

    ``encoder_stats`` carries per-frame x264 stats-file records when the
    request was driven through :func:`run_encode_with_stats` and the
    codec adapter declares ``supports_encoder_stats = True``. Empty
    tuple otherwise — corpus rows always populate the schema, so
    downstream readers see ``0.0`` aggregates rather than missing keys.
    """

    request: EncodeRequest
    encode_size_bytes: int
    encode_time_ms: float
    encoder_version: str
    ffmpeg_version: str
    exit_status: int
    stderr_tail: str
    encoder_stats: tuple[PerFrameStats, ...] = ()


def _legacy_codec_args(encoder: str, preset: str, quality: int) -> list[str]:
    """Fallback ``-c:v ... -preset <p> -crf <q>`` shape (HP-1).

    Used when an ``EncodeRequest`` names an encoder not in the
    adapter registry, or when a registered adapter doesn't yet
    expose ``ffmpeg_codec_args``. Mirrors the historic libx264-only
    argv shape so callers that bypass the registry still produce
    something invocable.
    """
    return ["-c:v", encoder, "-preset", preset, "-crf", str(quality)]


def _resolve_codec_args(req: "EncodeRequest") -> list[str]:
    """Resolve the codec-specific argv slice for ``req``.

    Routes through the codec-adapter registry per ADR-0237 / ADR-0326
    (HP-1): every adapter exposes ``ffmpeg_codec_args(preset, quality)``
    that returns the ``-c:v ...`` slice in the codec-correct shape (e.g.
    ``-cpu-used`` for libaom-av1, ``-cq`` for NVENC, ``-global_quality``
    for QSV). When the encoder isn't in the registry (or the adapter
    is a legacy stub without ``ffmpeg_codec_args``) we fall back to
    the historic libx264 shape so existing callers keep working.
    """
    # Local import: ``codec_adapters`` imports ``encode`` indirectly
    # through downstream modules in some test paths; the late binding
    # keeps the dependency one-way at module load. The import statement
    # also re-resolves ``get_adapter`` from the package on every call,
    # so tests that ``mock.patch.object(codec_adapters, "get_adapter", ...)``
    # see the patched callable.
    try:
        from . import codec_adapters as _ca
    except ImportError:  # pragma: no cover - defensive
        return _legacy_codec_args(req.encoder, req.preset, req.crf)

    try:
        adapter = _ca.get_adapter(req.encoder)
    except KeyError:
        return _legacy_codec_args(req.encoder, req.preset, req.crf)

    fn = getattr(adapter, "ffmpeg_codec_args", None)
    if fn is None:
        return _legacy_codec_args(req.encoder, req.preset, req.crf)

    args = list(fn(req.preset, req.crf))
    # Append adapter-level extra_params (codec-specific flags that are
    # orthogonal to quality/preset — e.g. -svtav1-params for SVT-AV1,
    # -row-mt for libaom-av1, -b:v 0 for VBR-mode encoders).
    extra_fn = getattr(adapter, "extra_params", None)
    if extra_fn is not None:
        import inspect  # noqa: PLC0415

        sig = inspect.signature(extra_fn)
        if len(sig.parameters) >= 2:
            extra = extra_fn(req.preset, req.crf)
        else:
            extra = extra_fn()
        if extra:
            # extra_params may be a flat tuple of strings or a tuple of
            # (flag, value) pairs; normalise both shapes.
            if isinstance(extra[0], tuple):
                for flag, val in extra:
                    args.extend([flag, val])
            else:
                args.extend(extra)
    return args


def build_ffmpeg_command(req: EncodeRequest, ffmpeg_bin: str = "ffmpeg") -> list[str]:
    """Compose the ffmpeg argv for a single encode.

    Pure function — no I/O — so tests can pin the exact command line.

    When ``req.sample_clip_seconds > 0``, ``-ss <start> -t <N>`` are
    inserted as **input-side** options (before ``-i``) so FFmpeg fast-
    seeks the raw YUV by skipping ``start * framerate`` frame-sized
    byte chunks. Output-side seeking would still decode (and the
    rawvideo demuxer would still read) the full source, defeating the
    speedup.

    Phase F (ADR-0333): when ``req.pass_number != 0`` the adapter's
    ``two_pass_args`` argv is spliced in before ``extra_params``; pass
    1 redirects the encoded output to ``-f null -`` (avoiding writing
    a useless pass-1 mp4) while pass 2 keeps the requested
    ``req.output`` destination.

    The codec-specific argv slice (``-c:v ...``) is delegated to the
    codec adapter's ``ffmpeg_codec_args`` per HP-1 / ADR-0326 so
    non-x264 codecs get their correct flags (e.g. ``-cpu-used`` for
    libaom-av1, ``-cq`` for NVENC, ``-global_quality`` for QSV). The
    legacy ``-c:v <enc> -preset <p> -crf <q>`` shape stays available
    as a fallback for unregistered encoders.
    """
    cmd: list[str] = [ffmpeg_bin, "-y", "-hide_banner", "-loglevel", "info"]
    # BBB e2e v6 Bug #V6-1 (ADR-0506): when the caller didn't opt into
    # sample-clip mode but did bind ``duration_s`` (via the ladder /
    # corpus ``--duration`` flag), honour it as an input-side ``-t``
    # so the encode is bounded to the analysed window. The reference
    # leg is already clipped by ``_maybe_decode_reference``; without
    # this guard the encode would process the full source (9 min of
    # BBB instead of 10 s) burning ~9x wall time per cell on long
    # sources.
    fallback_duration = (
        float(req.duration_s) if req.sample_clip_seconds <= 0.0 and req.duration_s > 0.0 else 0.0
    )
    if req.source_is_container:
        # Container source (mkv/mp4/…): let ffmpeg auto-detect format.
        # -ss/-t go before -i for fast input-seek on compressed streams.
        if req.sample_clip_seconds > 0.0:
            cmd.extend(["-ss", f"{req.sample_clip_start_s}"])
            cmd.extend(["-t", f"{req.sample_clip_seconds}"])
        elif fallback_duration > 0.0:
            cmd.extend(["-t", f"{fallback_duration}"])
        cmd.extend(["-i", str(req.source)])
    else:
        # Raw YUV source: must tell ffmpeg the format explicitly.
        cmd.extend(
            [
                "-f",
                "rawvideo",
                "-pix_fmt",
                req.pix_fmt,
                "-s",
                f"{req.width}x{req.height}",
                "-r",
                f"{req.framerate}",
            ]
        )
        if req.sample_clip_seconds > 0.0:
            # Input-side -ss / -t — fast-seek for raw YUV.
            cmd.extend(["-ss", f"{req.sample_clip_start_s}"])
            cmd.extend(["-t", f"{req.sample_clip_seconds}"])
        elif fallback_duration > 0.0:
            cmd.extend(["-t", f"{fallback_duration}"])
        cmd.extend(["-i", str(req.source)])
    cmd.extend(_resolve_codec_args(req))

    # Phase F: 2-pass argv from the codec adapter, when requested.
    if req.pass_number != 0:
        if req.stats_path is None:
            raise ValueError("build_ffmpeg_command: pass_number != 0 requires stats_path")
        # Lazy import to avoid the codec_adapters import cost on
        # plain single-pass paths and to keep the module import
        # graph identical for the legacy fast path.
        from .codec_adapters import get_adapter

        adapter = get_adapter(req.encoder)
        if not getattr(adapter, "supports_two_pass", False):
            raise ValueError(
                f"build_ffmpeg_command: encoder {req.encoder!r} does not "
                "support 2-pass encoding (supports_two_pass = False)"
            )
        cmd.extend(adapter.two_pass_args(req.pass_number, req.stats_path))

    cmd.extend(req.extra_params)

    if req.pass_number == 1:
        # Pass 1 only writes the stats file; the encoded bitstream is
        # discarded via the null muxer. Saves I/O + disk space (some
        # codecs emit hundreds of MB on long sources).
        cmd.extend(["-f", "null", "-"])
    else:
        cmd.append(str(req.output))
    return cmd


_FFMPEG_VERSION_RE = re.compile(r"ffmpeg version (\S+)")
# x264 banner formats:
#   classic: "x264 - core 164 r3094 bfc87b7" (libx264 banner)
#   fallback: ffmpeg's "[libx264 @ 0x55] using SAR=…" lines carry no
#   version; probe the meson/ffconfigure-emitted "libx264 N.NNN.NNNN" /
#   "x264-core NNN" alt strings the build also surfaces (ADR-0498
#   follow-up #7 — BBB e2e v2 logged "unknown" for x264 even with
#   the encoder running, because ``-hide_banner`` suppressed the
#   per-encoder banner on the build's ffmpeg).
# Accepts both "x264 - core 164" (canonical libx264 banner) and the
# defensive "x264-core 164" variant that some downstream builds emit
# in their configure summary (ADR-0498 follow-up #7).
_X264_VERSION_RE = re.compile(r"x264\s*-?\s*core\s+(\d+)")
_X265_VERSION_RE = re.compile(r"x265 \[info\]: HEVC encoder version (\S+)")
_LIBVPX_VP9_VERSION_RE = re.compile(r"\[libvpx-vp9 @ [^\]]+\]\s+v(\S+)")
# SVT-AV1 banner formats across versions:
#   older: "SVT-AV1 ENCODER v1.7.0"
#   newer: "Svt[info]:SVT-AV1 Encoder Lib v2.1.0"
#   v1.x: "SVT [info]: SVT-AV1 Encoder Lib v1.7.0"
_SVTAV1_VERSION_RE = re.compile(r"SVT-AV1 Encoder(?:\s+Lib)?\s+v(\S+)", re.IGNORECASE)
# libaom-av1: FFmpeg emits "[libaom-av1 @ 0x...] libaom-av1 v3.x.y" or
#   "[libaom @ 0x...] AOM version: 3.x.y" depending on FFmpeg vintage.
_LIBAOM_VERSION_RE = re.compile(
    r"\[libaom(?:-av1)?\s*@\s*[^\]]+\]\s+(?:libaom-av1\s+v|AOM version:\s*)(\S+)",
    re.IGNORECASE,
)
# VVenC: FFmpeg emits "[libvvenc @ 0x...] VVenC v1.14.0" or
#   "[libvvenc @ 0x...] Fraunhofer VVC/H.266 Encoder VVenC v1.14.0"
_LIBVVENC_VERSION_RE = re.compile(
    r"\[libvvenc\s*@\s*[^\]]+\]\s+(?:Fraunhofer\s+VVC/H\.266\s+Encoder\s+)?VVenC\s+v(\S+)",
    re.IGNORECASE,
)


def parse_versions(stderr: str, encoder: str = "libx264") -> tuple[str, str]:
    """Return (ffmpeg_version, encoder_version) extracted from stderr.

    ``encoder`` selects the per-codec version regex. Supported values
    match the codec_adapters registry: ``libx264`` (default), ``libx265``,
    ``libsvtav1``, ``libvpx-vp9``, ``libaom-av1``, ``libvvenc``, and any
    HW encoder token (h264_nvenc, hevc_amf, …).
    HW encoders don't advertise a version in stderr; the encoder token
    string is returned verbatim so corpus rows carry a stable identifier.
    ``libaom-av1`` and ``libvvenc`` emit a version banner when available;
    the encoder name is used as the stable fallback when the banner is
    absent (e.g. builds that suppress per-encoder output).

    Returns ``("unknown", "unknown")`` for missing matches rather than
    raising — corpus rows record what we can detect and move on.
    """
    ffm = _FFMPEG_VERSION_RE.search(stderr)
    ffm_str = ffm.group(1) if ffm else "unknown"

    enc_str: str
    _DEFAULT_ENCODER = "libx264"
    if encoder == _DEFAULT_ENCODER or not encoder:
        # Auto-detect from stderr when the caller didn't pass an explicit
        # encoder override (i.e. still at default "libx264"): x264 banner
        # takes priority (it appears first in multi-codec logs), then x265,
        # then SVT-AV1. If no banner is found, return "unknown".
        m_x4 = _X264_VERSION_RE.search(stderr)
        if m_x4:
            enc_str = f"libx264-{m_x4.group(1)}"
        else:
            m_x5 = _X265_VERSION_RE.search(stderr)
            if m_x5:
                enc_str = f"libx265-{m_x5.group(1)}"
            else:
                m_sv = _SVTAV1_VERSION_RE.search(stderr)
                enc_str = f"libsvtav1-{m_sv.group(1)}" if m_sv else "unknown"
    elif encoder == "libx265":
        m = _X265_VERSION_RE.search(stderr)
        enc_str = f"libx265-{m.group(1)}" if m else "unknown"
    elif encoder in ("libsvtav1", "libsvtav1-vbr"):
        m = _SVTAV1_VERSION_RE.search(stderr)
        enc_str = f"libsvtav1-{m.group(1)}" if m else "unknown"
    elif encoder == "libvpx-vp9":
        m = _LIBVPX_VP9_VERSION_RE.search(stderr)
        enc_str = f"libvpx-vp9-{m.group(1)}" if m else "unknown"
    elif encoder == "libaom-av1":
        # libaom emits a version banner in the per-run stderr when FFmpeg
        # is built with verbose encoder logging. Fall back to the stable
        # adapter name when the banner is absent (quiet builds).
        m = _LIBAOM_VERSION_RE.search(stderr)
        enc_str = f"libaom-av1-{m.group(1)}" if m else "libaom-av1"
    elif encoder == "libvvenc":
        # VVenC emits "VVenC v<version>" via the FFmpeg libvvenc wrapper.
        # Fall back to the stable adapter name when the banner is absent.
        m = _LIBVVENC_VERSION_RE.search(stderr)
        enc_str = f"libvvenc-{m.group(1)}" if m else "libvvenc"
    else:
        # Known HW encoder tokens (nvenc/amf/qsv/videotoolbox): no version
        # string in stderr; return the token as the stable identifier.
        # Completely unknown names return "unknown".
        _HW_TOKENS = (
            "_nvenc",
            "_amf",
            "_qsv",
            "_videotoolbox",
        )
        if any(tok in encoder for tok in _HW_TOKENS):
            enc_str = encoder
        else:
            enc_str = "unknown"

    return ffm_str, enc_str


def run_encode(
    req: EncodeRequest,
    *,
    ffmpeg_bin: str = "ffmpeg",
    runner: object | None = None,
    encoder_runner: object | None = None,
) -> EncodeResult:
    """Drive ffmpeg to produce ``req.output``.

    ``runner`` / ``encoder_runner`` are aliases — both default to
    ``subprocess.run`` and are parameterised so tests inject a stub.
    ``encoder_runner`` is the newer name; ``runner`` is kept for
    backward compatibility with existing corpus.py callers.
    """
    cmd = build_ffmpeg_command(req, ffmpeg_bin=ffmpeg_bin)
    runner_fn = encoder_runner or runner or subprocess.run
    started = time.monotonic()
    completed = runner_fn(  # type: ignore[operator]
        cmd, capture_output=True, text=True, check=False
    )
    elapsed_ms = (time.monotonic() - started) * 1000.0

    stderr = getattr(completed, "stderr", "") or ""
    rc = int(getattr(completed, "returncode", 1))

    size = 0
    # Pass 1 of a 2-pass encode writes only the stats file; the
    # encoded bitstream is discarded via -f null - and req.output is
    # not produced. Skip the size probe for pass 1 to avoid spurious
    # zeros tripping callers that interpret a zero-size on a non-pass-1
    # encode as failure.
    if rc == 0 and req.pass_number != 1 and req.output.exists():
        size = req.output.stat().st_size

    ffmpeg_v, encoder_v = parse_versions(stderr, encoder=req.encoder)
    # ADR-0498 follow-up #7: when the encode succeeded but the per-
    # encoder banner didn't appear in the captured stderr (modern
    # FFmpeg builds suppress it under ``-hide_banner``), fall back
    # to ``ffmpeg -version``'s ``configuration: ... --enable-libx264 ...``
    # / ``libavcodec N.N.N`` lines. The fallback runs at most once
    # per process via the LRU cache below; mocks in tests already
    # patch ``runner_fn`` so the fallback is a no-op unless the
    # underlying subprocess is real.
    if rc == 0 and encoder_v == "unknown":
        probed = _probe_encoder_version_from_ffmpeg(ffmpeg_bin, req.encoder, runner_fn)
        if probed:
            encoder_v = probed
    return EncodeResult(
        request=req,
        encode_size_bytes=size,
        encode_time_ms=elapsed_ms,
        encoder_version=encoder_v,
        ffmpeg_version=ffmpeg_v,
        exit_status=rc,
        stderr_tail=_tail(stderr, n=2048),
    )


def _tail(text: str, n: int) -> str:
    if len(text) <= n:
        return text
    return text[-n:]


# Fallback "encoder version" probe — populated lazily on first miss
# from a real ``ffmpeg -version`` run. Keyed by (ffmpeg_bin, encoder)
# so a test that swaps the binary path between probes still gets a
# fresh probe. ADR-0498 follow-up #7 (BBB e2e v2).
_PROBE_CACHE: dict[tuple[str, str], str] = {}

# Map encoder names to configure-line patterns for parsing the
# ``ffmpeg -version`` output. The configure summary looks like::
#
#   configuration: --prefix=/usr ... --enable-libx264 --enable-libsvtav1 ...
#
# which carries no per-encoder version. The libavcodec banner that
# follows also carries no encoder version. For these codecs we settle
# for an ``"<encoder>-enabled"`` marker so consumers at least know the
# encoder was compiled in. ADR-0498 follow-up #7 extends this set to
# cover x265 and libvpx so the ``EncoderInfo.codec_detected`` field is
# populated for all three software encoder families.
_VERSION_PROBE_PATTERNS: dict[str, re.Pattern] = {
    "libx264": re.compile(r"--enable-libx264"),
    "libsvtav1": re.compile(r"--enable-libsvtav1"),
    "libx265": re.compile(r"--enable-libx265"),
    "libvpx-vp9": re.compile(r"--enable-libvpx"),
    # libaom and VVenC configure flags (ADR-1077).
    "libaom-av1": re.compile(r"--enable-libaom"),
    "libvvenc": re.compile(r"--enable-libvvenc"),
}


@dataclasses.dataclass(frozen=True)
class EncoderInfo:
    """Structured encoder availability record from ``ffmpeg -version``.

    ``encoder`` is the FFmpeg codec name (e.g. ``libx264``).
    ``codec_detected`` is ``True`` when the configure summary confirms
    the codec was compiled into the ffmpeg binary (``--enable-<codec>``
    present in the ``configuration:`` line).  ``version_label`` carries
    the human-readable token returned by
    :func:`_probe_encoder_version_from_ffmpeg` (e.g. ``"libx264-enabled"``).

    ADR-0498 follow-up #7: this dataclass replaces the bare string
    return from the probe so callers can gate codec-stats capture and
    report generation on ``codec_detected`` without re-parsing the
    version string.
    """

    encoder: str
    codec_detected: bool
    version_label: str


def _probe_encoder_version_from_ffmpeg(ffmpeg_bin: str, encoder: str, runner_fn: object) -> str:
    """Return a best-effort version label, or ``""`` when nothing parseable.

    The function returns a short stable label (``libx264-enabled`` /
    ``libsvtav1-enabled`` / ``libx265-enabled`` / ``libvpx-vp9-enabled``)
    when ``ffmpeg -version``'s configuration line confirms the encoder is
    compiled in. Empty string keeps the caller's previous ``"unknown"``
    placeholder so existing tests that pin that exact value still pass.

    See also :func:`probe_encoder_info` for a structured
    :class:`EncoderInfo` return when callers need the ``codec_detected``
    boolean without reparsing the label string.
    """
    pattern = _VERSION_PROBE_PATTERNS.get(encoder)
    if pattern is None:
        return ""
    key = (ffmpeg_bin, encoder)
    if key in _PROBE_CACHE:
        return _PROBE_CACHE[key]
    try:
        completed = runner_fn(  # type: ignore[operator]
            [ffmpeg_bin, "-version"], capture_output=True, text=True, check=False
        )
    except (OSError, ValueError):
        _PROBE_CACHE[key] = ""
        return ""
    out = (getattr(completed, "stdout", "") or "") + (getattr(completed, "stderr", "") or "")
    if pattern.search(out):
        label = f"{encoder}-enabled"
    else:
        label = ""
    _PROBE_CACHE[key] = label
    return label


def probe_encoder_info(
    ffmpeg_bin: str, encoder: str, runner_fn: object | None = None
) -> EncoderInfo:
    """Return structured encoder availability info from ``ffmpeg -version``.

    Wraps :func:`_probe_encoder_version_from_ffmpeg` and returns an
    :class:`EncoderInfo` with ``codec_detected = True`` when the
    configure summary confirms the encoder is compiled in. Returns
    ``codec_detected = False`` for unknown encoders (not in
    ``_VERSION_PROBE_PATTERNS``) or when the configure line does not
    include the ``--enable-<codec>`` flag.

    ADR-0498 follow-up #7: callers that previously checked
    ``version_label != "unknown"`` can now use the boolean
    ``codec_detected`` field directly.
    """
    _runner = runner_fn if runner_fn is not None else subprocess.run
    label = _probe_encoder_version_from_ffmpeg(ffmpeg_bin, encoder, _runner)
    detected = bool(label)
    return EncoderInfo(encoder=encoder, codec_detected=detected, version_label=label or "unknown")


def build_pass1_stats_command(
    req: EncodeRequest, stats_prefix: Path, ffmpeg_bin: str = "ffmpeg"
) -> list[str]:
    """Compose the FFmpeg argv for a stats-only pass-1 invocation.

    Mirrors :func:`build_ffmpeg_command` but appends
    ``-pass 1 -passlogfile <prefix>`` and writes the bitstream to
    ``-f null /dev/null`` — the encoder still runs the full RD loop
    (and thus emits the stats file) but we skip muxing / writing the
    output. The stats file lands at ``<prefix>-0.log`` (and an
    ``mbtree`` sidecar at ``<prefix>-0.log.mbtree`` which we ignore).

    Bug #V8-A (ADR-0508): mirrors the ``build_ffmpeg_command`` V6-1
    fallback — when the caller bound ``req.duration_s`` (via the
    ladder / CLI ``--duration`` flag) without opting into sample-clip
    mode, the pass-1 invocation must honour that window too. Without
    this guard, ``ladder --duration 5`` against a 9-minute source
    would run pass-1 stats over all 9 minutes (then pass-2 honoured
    the window via :func:`build_ffmpeg_command`), so each cell still
    burned >60x the requested wall time on the stats sweep alone.
    """
    # V6-1 / #1266 follow-up: `build_pass1_stats_command` lost the
    # duration_s fallback that `build_ffmpeg_command` got — the ladder's
    # pass-1 stats sweep still ran on the full source. Apply the same
    # precedence: sample_clip_seconds wins (with --ss start), else
    # plain --t duration_s, else no clip.
    fallback_duration = (
        float(req.duration_s) if req.sample_clip_seconds <= 0.0 and req.duration_s > 0.0 else 0.0
    )
    cmd = [ffmpeg_bin, "-y", "-hide_banner", "-loglevel", "info"]
    if req.source_is_container:
        if req.sample_clip_seconds > 0.0:
            cmd.extend(["-ss", f"{req.sample_clip_start_s}"])
            cmd.extend(["-t", f"{req.sample_clip_seconds}"])
        elif fallback_duration > 0.0:
            cmd.extend(["-t", f"{fallback_duration}"])
        cmd.extend(["-i", str(req.source)])
    else:
        cmd.extend(
            [
                "-f",
                "rawvideo",
                "-pix_fmt",
                req.pix_fmt,
                "-s",
                f"{req.width}x{req.height}",
                "-r",
                f"{req.framerate}",
            ]
        )
        if req.sample_clip_seconds > 0.0:
            cmd.extend(["-ss", f"{req.sample_clip_start_s}"])
            cmd.extend(["-t", f"{req.sample_clip_seconds}"])
        elif fallback_duration > 0.0:
            cmd.extend(["-t", f"{fallback_duration}"])
        cmd.extend(["-i", str(req.source)])
    cmd.extend(
        [
            "-c:v",
            req.encoder,
            "-preset",
            req.preset,
            "-crf",
            str(req.crf),
        ]
    )
    cmd.extend(req.extra_params)
    cmd.extend(
        [
            "-pass",
            "1",
            "-passlogfile",
            str(stats_prefix),
            "-f",
            "null",
            os.devnull,
        ]
    )
    return cmd


def _stats_file_for(prefix: Path) -> Path:
    """Path FFmpeg writes the x264 stats file to under ``-passlogfile``.

    FFmpeg appends ``-0.log`` for the first (and in our case only)
    video stream. Mirroring this convention here keeps the parser
    fixture-agnostic.
    """
    return prefix.parent / f"{prefix.name}-0.log"


def run_encode_with_stats(
    req: EncodeRequest,
    *,
    ffmpeg_bin: str = "ffmpeg",
    runner: object | None = None,
    capture_stats: bool = True,
    stats_dir: Path | None = None,
) -> EncodeResult:
    """Encode ``req`` and (optionally) capture x264 pass-1 stats.

    When ``capture_stats`` is True (the corpus default for codecs that
    declare ``supports_encoder_stats``), this runs FFmpeg twice:

    1. Stats-only pass-1 (``-pass 1 -passlogfile <tmp> -f null``).
       Reads the ``<tmp>-0.log`` file, parses it into
       :class:`PerFrameStats` records, and discards the temp.
    2. The regular CRF encode via :func:`run_encode`, which produces
       the bitstream the corpus scores.

    The result is the regular :class:`EncodeResult` with the
    ``encoder_stats`` tuple populated. Per ADR-0332 this doubles the
    per-encode wall-clock cost — that is the documented trade-off for
    closing the loop on the encoder's RC ledger.

    When ``capture_stats=False`` the function delegates straight to
    :func:`run_encode` and returns an empty stats tuple.
    """
    if not capture_stats:
        return run_encode(req, ffmpeg_bin=ffmpeg_bin, runner=runner)

    cleanup: list[Path] = []
    base_dir = Path(stats_dir) if stats_dir is not None else Path(tempfile.gettempdir())
    base_dir.mkdir(parents=True, exist_ok=True)
    # Use a deterministic prefix for the temp file so test fixtures can
    # pre-seed a stats file at the expected path.
    prefix = base_dir / f"vmaftune_stats_{os.getpid()}_{req.output.stem}"

    try:
        cmd = build_pass1_stats_command(req, prefix, ffmpeg_bin=ffmpeg_bin)
        runner_fn = runner or subprocess.run
        runner_fn(cmd, capture_output=True, text=True, check=False)  # type: ignore[operator]
        stats_path = _stats_file_for(prefix)
        frames = tuple(parse_stats_file(stats_path))
        cleanup.append(stats_path)
        cleanup.append(prefix.parent / f"{prefix.name}-0.log.mbtree")

        result = run_encode(req, ffmpeg_bin=ffmpeg_bin, runner=runner)
        return dataclasses.replace(result, encoder_stats=frames)
    finally:
        for p in cleanup:
            try:
                p.unlink()
            except (OSError, FileNotFoundError):
                pass


def bitrate_kbps(size_bytes: int, duration_s: float) -> float:
    """File-size-derived bitrate. 0 if duration is non-positive."""
    if duration_s <= 0:
        return 0.0
    return (size_bytes * 8.0 / 1000.0) / duration_s


def iter_grid(presets: Sequence[str], crfs: Sequence[int]) -> list[tuple[str, int]]:
    """Cartesian product of presets x crfs as a deterministic list."""
    return [(p, c) for p in presets for c in crfs]


def _stats_path_for(req: EncodeRequest, scratch_dir: Path) -> Path:
    """Build a per-encode unique stats-file path under ``scratch_dir``.

    The stats file name embeds the source stem, encoder, preset, and
    CRF so a debug session can correlate it back to the encode that
    produced it. A short uuid suffix prevents collisions when the
    same (src, codec, preset, crf) is run more than once in parallel.
    """
    stem = f"{req.source.stem}__{req.encoder}__{req.preset}__crf{req.crf}__{uuid.uuid4().hex[:8]}"
    return scratch_dir / f"{stem}.stats"


def _two_pass_cleanup_candidates(stats_path: Path) -> tuple[Path, ...]:
    """Files known encoders may create for a two-pass stats prefix."""
    ffmpeg_stream_log = _stats_file_for(stats_path)
    return (
        stats_path,
        stats_path.with_suffix(stats_path.suffix + ".cutree"),
        ffmpeg_stream_log,
        ffmpeg_stream_log.with_suffix(ffmpeg_stream_log.suffix + ".mbtree"),
    )


def run_two_pass_encode(
    req: EncodeRequest,
    *,
    ffmpeg_bin: str = "ffmpeg",
    runner: object | None = None,
    scratch_dir: Path | None = None,
    on_unsupported: str = "fallback",
) -> EncodeResult:
    """Drive a 2-pass ffmpeg encode (Phase F / ADR-0333).

    Runs the encoder twice with the codec adapter's ``two_pass_args``
    spliced into each invocation. Pass 1 redirects to ``-f null -``
    (no output file); pass 2 writes ``req.output``. The stats file
    lives in ``scratch_dir`` (default: a fresh ``tempfile.mkdtemp``)
    and is removed after pass 2 completes regardless of exit status.

    Returns one :class:`EncodeResult` representing the combined op:

    - ``encode_size_bytes`` — pass-2 output size.
    - ``encode_time_ms`` — sum of pass-1 + pass-2 wall time.
    - ``encoder_version`` / ``ffmpeg_version`` — pass-2 stderr (which
      carries the actual encode banner; pass 1 emits the same lines
      but pass 2 is the canonical source).
    - ``exit_status`` — first non-zero of {pass 1, pass 2}, else 0.
    - ``stderr_tail`` — pass-2 stderr tail (pass-1 failures get
      surfaced via ``exit_status``).

    ``on_unsupported`` controls behaviour when the request's encoder
    has ``supports_two_pass = False``:

    - ``"fallback"`` (default) — log a one-line stderr warning and
      run a single-pass encode (returning that result). Mirrors the
      saliency.py "unsupported ROI encoder, fall back to plain encode" precedent.
    - ``"raise"`` — raise :class:`ValueError`. For callers that want
      to fail loudly rather than silently degrade.
    """
    from .codec_adapters import get_adapter

    adapter = get_adapter(req.encoder)
    if not getattr(adapter, "supports_two_pass", False):
        msg = (
            f"vmaf-tune: encoder {req.encoder!r} does not support 2-pass "
            "encoding; falling back to single-pass."
        )
        if on_unsupported == "raise":
            raise ValueError(msg)
        if on_unsupported != "fallback":
            raise ValueError(
                f"run_two_pass_encode: unknown on_unsupported={on_unsupported!r}; "
                "expected 'fallback' or 'raise'"
            )
        sys.stderr.write(msg + "\n")
        return run_encode(req, ffmpeg_bin=ffmpeg_bin, runner=runner)

    own_scratch = scratch_dir is None
    if scratch_dir is None:
        scratch_dir = Path(tempfile.mkdtemp(prefix="vmaftune-2pass-"))

    stats_path = _stats_path_for(req, scratch_dir)
    pass1_req = dataclasses.replace(req, pass_number=1, stats_path=stats_path)
    pass2_req = dataclasses.replace(req, pass_number=2, stats_path=stats_path)

    try:
        pass1 = run_encode(pass1_req, ffmpeg_bin=ffmpeg_bin, runner=runner)
        if pass1.exit_status != 0:
            # Don't bother with pass 2 if pass 1 failed; surface the
            # pass-1 failure in the EncodeResult (with a clarifying
            # tail) so the caller can disambiguate from a pass-2 fault.
            return dataclasses.replace(
                pass1,
                request=req,  # report against the user-supplied request
                stderr_tail=f"[pass 1 failed]\n{pass1.stderr_tail}",
            )
        pass2 = run_encode(pass2_req, ffmpeg_bin=ffmpeg_bin, runner=runner)
        combined_status = pass2.exit_status  # pass1 was 0 by branch above
        return EncodeResult(
            request=req,
            encode_size_bytes=pass2.encode_size_bytes,
            encode_time_ms=pass1.encode_time_ms + pass2.encode_time_ms,
            encoder_version=pass2.encoder_version,
            ffmpeg_version=pass2.ffmpeg_version,
            exit_status=combined_status,
            stderr_tail=pass2.stderr_tail,
        )
    finally:
        # Remove known stats artefacts. libx265 writes ``<stats>`` and
        # may add ``<stats>.cutree``; FFmpeg's generic passlogfile path
        # (used by libx264) writes ``<stats>-0.log`` plus an optional
        # mbtree sidecar.
        for candidate in _two_pass_cleanup_candidates(stats_path):
            try:
                candidate.unlink()
            except (OSError, FileNotFoundError):
                pass
        if own_scratch:
            try:
                # Best-effort cleanup; if anything remains the OS will
                # garbage-collect /tmp eventually.
                for child in scratch_dir.iterdir():
                    try:
                        child.unlink()
                    except OSError:
                        pass
                scratch_dir.rmdir()
            except OSError:
                pass
