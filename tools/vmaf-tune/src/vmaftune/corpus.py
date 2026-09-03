# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Phase A corpus orchestrator.

Sweeps a (preset, crf) grid against one or more raw YUV references,
runs the encoder, scores each encode against the reference with the
libvmaf CLI, and writes one JSONL row per (source, preset, crf)
combination.

Schema lives in :mod:`vmaftune` (``CORPUS_ROW_KEYS``,
``SCHEMA_VERSION``). Phase B/C are downstream consumers — bumping the
schema is a coordinated change.

A 2-pass coarse-to-fine search is also exposed via
:func:`coarse_to_fine_search` for callers that only need to find the
smallest CRF that still meets a VMAF target. The full ``--crf-range
0:51:1`` grid wastes encode wall time once the target is bracketed;
coarse-to-fine visits ~15 points instead of 52 for the canonical
defaults (3.5x speedup) — see ADR-0296.
"""

from __future__ import annotations

import contextlib
import dataclasses
import json
import logging
import math
import os
import sys
import uuid
from collections.abc import Iterator, Sequence
from pathlib import Path
from typing import Any

from . import (
    CANONICAL6_FEATURES,
    CANONICAL6_MEAN_KEYS,
    CANONICAL6_STD_KEYS,
    CORPUS_ROW_KEYS,
    SCHEMA_VERSION,
)
from .codec_adapters import get_adapter
from .encode import (
    EncodeRequest,
    bitrate_kbps,
    run_encode,
    run_encode_with_stats,
    run_two_pass_encode,
)
from .encoder_stats import aggregate_stats
from .hdr import HdrInfo, detect_hdr, hdr_codec_args, select_hdr_vmaf_model
from .per_shot import ShotMetadata, _detect_shots_with_status, summarise_shots
from .score import ScoreRequest, ScoreResult, run_score

# Resolve ai/src so aiutils helpers are importable without requiring an
# editable install of the ai package.  The repo layout is:
#   tools/vmaf-tune/src/vmaftune/corpus.py  (parents[4] = repo root)
_REPO_ROOT = Path(__file__).resolve().parents[4]
_AI_SRC = _REPO_ROOT / "ai" / "src"
if str(_AI_SRC) not in sys.path:
    sys.path.insert(0, str(_AI_SRC))

from aiutils.file_utils import sha256 as _sha256_file
from aiutils.time_utils import now_iso_8601 as _utc_now_iso

from .defaultmodel import DEFAULT_MODEL

_LOG = logging.getLogger(__name__)

# Suffixes the vmaf CLI accepts as raw YUV without a prior ffmpeg decode
# step. ADR-0499 / BBB e2e v3 Bug #V3-B: ``.y4m`` was previously listed
# here on the assumption the vmaf CLI auto-detects Y4M containers from
# the extension. It does not — vmaf-tune always passes ``--width`` /
# ``--height`` / ``--pixel_format`` / ``--bitdepth`` (see
# :func:`vmaftune.score.build_vmaf_command`), which flips the CLI's
# ``use_yuv`` flag (core/tools/cli_parse.c) and routes both inputs
# through ``raw_input_open``. Y4M files then trip the file-size-mismatch
# guard inside ``raw_input_open``. The empty-suffix entry is kept for
# operators who name raw YUV without a ``.yuv`` extension (a long-
# standing convention in fixture trees); they get correct behaviour
# because ``--width`` / etc. already pin the geometry.
_VMAF_RAW_SUFFIXES: frozenset[str] = frozenset({".yuv", ""})


def _decode_source_to_yuv(
    source: Path,
    *,
    destination: Path,
    pix_fmt: str,
    duration_s: float,
    ffmpeg_bin: str,
    runner: object,
    target_width: int | None = None,
    target_height: int | None = None,
    source_width: int | None = None,
    source_height: int | None = None,
    source_framerate: float | None = None,
    source_is_raw: bool = False,
) -> int:
    """Run ``ffmpeg -i source -f rawvideo -pix_fmt pix_fmt destination``.

    Shared building block for both :func:`_maybe_decode_distorted` and
    :func:`_maybe_decode_reference`. The two callers differ only in
    where they place the output file and what they do on failure;
    centralising the argv keeps the ``-t`` clamp and pix_fmt selection
    in one place (BBB e2e v2 Bug #v2-A + v3 Bug #V3-B).

    BBB e2e v4 Bug #V4-B (ADR-0501): when ``target_width`` and
    ``target_height`` are both set, append ``-vf scale=W:H`` so the
    decoded raw YUV lands at the requested rendition geometry rather
    than the source's native geometry. This is what lets the libvmaf
    CLI read both reference and distorted as the *same* width/height
    on a cross-resolution rung — without it, the binary mis-parses a
    1920x1080 reference as a 1280x720 frame and produces nonsense
    VMAF (~21 instead of ~93). ``None`` for either field skips the
    filter and preserves the legacy auto-detect-source-geometry
    behaviour.

    BBB e2e v6 Bug #V6-2 (ADR-0506): when ``source_is_raw=True``,
    insert the demuxer-side ``-f rawvideo -pix_fmt … -s WxH -r FR``
    block BEFORE ``-i`` so ffmpeg can parse the raw YUV — without
    those flags ffmpeg auto-detects nothing and refuses the input
    ("Invalid data found when processing input"). The v4 cross-res
    scale path passed raw-YUV references through this helper without
    the raw input flags, causing the 854x480 rung of a 1280x720
    raw source to error with "default sampler produced no scorable
    encodes". ``source_is_raw=False`` (default) preserves the
    container-source auto-detect path.
    """
    destination.parent.mkdir(parents=True, exist_ok=True)
    cmd: list[str] = [
        ffmpeg_bin,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
    ]
    if source_is_raw:
        # BBB e2e v6 Bug #V6-2: tell ffmpeg the demuxer geometry on
        # the input side so raw YUV is parseable. ``source_width`` /
        # ``source_height`` are required for raw decode; framerate
        # defaults to 24 (matches ``_default_sampler`` legacy) when
        # not bound.
        if source_width is None or source_height is None:
            raise ValueError(
                "_decode_source_to_yuv: source_is_raw=True requires "
                "source_width and source_height"
            )
        cmd.extend(
            [
                "-f",
                "rawvideo",
                "-pix_fmt",
                pix_fmt,
                "-s",
                f"{int(source_width)}x{int(source_height)}",
                "-r",
                f"{float(source_framerate) if source_framerate is not None else 24.0}",
            ]
        )
    cmd.extend(["-i", str(source)])
    cmd.extend(["-f", "rawvideo", "-pix_fmt", pix_fmt])
    if target_width is not None and target_height is not None:
        cmd.extend(["-vf", f"scale={int(target_width)}:{int(target_height)}"])
    if duration_s > 0.0:
        cmd.extend(["-t", f"{float(duration_s)}"])
    cmd.append(str(destination))
    import subprocess as _sp

    run_fn = runner if callable(runner) else _sp.run
    completed = run_fn(cmd, capture_output=True, text=True, check=False)
    return int(getattr(completed, "returncode", 1))


def _maybe_decode_distorted(
    req: ScoreRequest,
    *,
    encode_dir: Path,
    ffmpeg_bin: str,
    runner: object,
) -> ScoreRequest:
    """Decode ``req.distorted`` to a raw YUV sidecar when it is a container.

    The vmaf CLI only accepts raw ``.yuv`` (see :data:`_VMAF_RAW_SUFFIXES`).
    Encoded outputs are containers (``mp4``, ``mkv``, …); they must be
    decoded before the scoring step. The decoded YUV file is placed next
    to the encode under ``encode_dir`` with a ``.decoded.yuv`` suffix so
    callers that keep encodes (``--keep-encodes``) can find it.

    Returns an updated :class:`ScoreRequest` pointing at the decoded path.
    When the distorted file is already raw, or when the decode fails, the
    original request is returned unchanged (the vmaf binary will then
    fail/crash on an undecodable input and the row will record
    ``exit_status != 0``).
    """
    if req.distorted.suffix.lower() in _VMAF_RAW_SUFFIXES:
        return req

    decoded = encode_dir / (req.distorted.stem + ".decoded.yuv")
    rc = _decode_source_to_yuv(
        req.distorted,
        destination=decoded,
        pix_fmt=req.pix_fmt,
        # BBB e2e v2 Bug #v2-A: cap the decoded YUV at the analysed
        # window length when the caller bound ``ScoreRequest.duration_s``.
        duration_s=float(getattr(req, "duration_s", 0.0)),
        ffmpeg_bin=ffmpeg_bin,
        runner=runner,
    )
    if rc == 0 and decoded.exists():
        return dataclasses.replace(req, distorted=decoded)
    _LOG.warning(
        "corpus: ffmpeg decode of %s failed (rc=%s); scoring will likely fail",
        req.distorted,
        rc,
    )
    return req


def _maybe_decode_reference(
    source: Path,
    *,
    encode_dir: Path,
    pix_fmt: str,
    duration_s: float,
    ffmpeg_bin: str,
    runner: object,
    target_width: int | None = None,
    target_height: int | None = None,
    source_width: int | None = None,
    source_height: int | None = None,
    source_framerate: float | None = None,
) -> tuple[Path, int]:
    """Decode a container reference to raw YUV once per ``iter_rows`` call.

    Mirror of :func:`_maybe_decode_distorted` for the reference leg.
    ADR-0499 / BBB e2e v3 Bug #V3-B: before this helper landed, only
    the encoded distorted output was decoded — the *reference* was
    handed to the vmaf CLI as-is. When the source is a container
    (``.mp4`` / ``.mkv``) or a Y4M file, ``raw_input_open`` reads the
    container bytes as raw planes and aborts with "file size mismatch"
    (Y4M) or "file too small for declared geometry" (MP4).

    ADR-0501 / BBB e2e v4 Bug #V4-B: when the *rung target* differs
    from the source's native geometry, the reference must be decoded
    *and downscaled* to the rung target so the libvmaf CLI reads both
    legs at the same width/height. Without the scale filter the binary
    silently mis-parses the planar bytes of a 1080p reference as a
    720p frame and emits a catastrophic ~21 VMAF instead of the true
    ~93. Callers pass ``target_width`` / ``target_height`` to engage
    the per-rung scale; both ``None`` (default) preserves the
    legacy native-geometry decode. The decoded sidecar's filename
    embeds the target dims so cross-resolution rungs in the same
    sweep don't collide on a stale path.

    Returns ``(reference_path, returncode)``. ``returncode == 0`` with
    the original ``source`` returned when the source is already raw
    (no work to do). A non-zero ``returncode`` with the original
    ``source`` returned signals a decode failure; callers should treat
    every (preset, crf) cell as failed rather than invoking the vmaf
    binary on an undecodable file. The decoded YUV file is placed
    under ``encode_dir`` with a ``.ref.decoded.yuv`` suffix so the
    same path can be reused across every cell in the sweep.
    """
    source_is_raw = source.suffix.lower() in _VMAF_RAW_SUFFIXES
    if source_is_raw and target_width is None and target_height is None:
        return source, 0
    if target_width is not None and target_height is not None:
        # Per-rung sidecar — embed WxH in the filename so multi-rung
        # sweeps don't collide on a stale decode (ADR-0501).
        decoded = encode_dir / (
            f"{source.stem}.ref.decoded.{int(target_width)}x{int(target_height)}.yuv"
        )
    else:
        decoded = encode_dir / (source.stem + ".ref.decoded.yuv")
    # Re-use a previous decode if one is already on disk for this
    # ``iter_rows`` call. The same source + same window length is
    # constant across every cell so a single decode suffices.
    if decoded.exists():
        return decoded, 0
    rc = _decode_source_to_yuv(
        source,
        destination=decoded,
        pix_fmt=pix_fmt,
        duration_s=duration_s,
        ffmpeg_bin=ffmpeg_bin,
        runner=runner,
        target_width=target_width,
        target_height=target_height,
        # BBB e2e v6 Bug #V6-2 (ADR-0506): when the source is raw YUV
        # (`.yuv` / no-suffix) the demuxer needs explicit format flags
        # — without them ffmpeg cannot parse raw planar bytes and the
        # cross-resolution rung's reference decode fails. ``source_*``
        # carry the caller-bound geometry (the corpus job's source
        # dims) so this helper can synthesise the demuxer ``-s WxH``.
        source_is_raw=source_is_raw,
        source_width=source_width,
        source_height=source_height,
        source_framerate=source_framerate,
    )
    if rc == 0 and decoded.exists():
        return decoded, 0
    _LOG.warning(
        "corpus: ffmpeg decode of reference %s failed (rc=%s); "
        "every (preset, crf) cell will record exit_status != 0",
        source,
        rc,
    )
    return source, rc if rc != 0 else 1


@dataclasses.dataclass(frozen=True)
class CorpusJob:
    """One source + a list of (preset, crf) cells to evaluate.

    ``width`` / ``height`` are the *rung target* (encode output and
    libvmaf score) dimensions. ``src_width`` / ``src_height`` (added
    2026-05-18, ADR-0498) are optional source-side overrides used when
    the raw-YUV source's actual geometry differs from the rung target:
    the encode pipe then tells ffmpeg the true source ``-s W:H`` and
    a ``-vf scale=W:H`` filter downscales to the rung target. Both
    ``None`` (default) keeps the legacy single-resolution behaviour
    where ``width / height`` serve as both source and rung dims —
    safe for container sources (ffmpeg auto-detects geometry) and
    raw YUV at the requested resolution.
    """

    source: Path
    width: int
    height: int
    pix_fmt: str
    framerate: float
    duration_s: float
    cells: tuple[tuple[str, int], ...]
    src_width: int | None = None
    src_height: int | None = None


@dataclasses.dataclass(frozen=True)
class CorpusOptions:
    """Knobs that govern a corpus run.

    ``sample_clip_seconds`` opts the run into sample-clip mode
    (ADR-0297): each grid point encodes the centre N-second window of
    the reference YUV instead of the full source, scoring the matching
    reference window via the libvmaf CLI's ``--frame_skip_ref`` /
    ``--frame_cnt``. ``0.0`` (default) keeps the legacy full-source
    behaviour. The encoded clip's bitrate and timing are reported as
    measured on the slice — Phase B/C should weight or filter rows on
    ``clip_mode`` rather than mixing sample and full rows blindly.
    """

    encoder: str = "libx264"
    output: Path = Path("corpus.jsonl")
    encode_dir: Path = Path(".workingdir2/encodes")
    vmaf_model: str = DEFAULT_MODEL
    ffmpeg_bin: str = "ffmpeg"
    vmaf_bin: str = "vmaf"
    keep_encodes: bool = False
    src_sha256: bool = True
    sample_clip_seconds: float = 0.0
    # ADR-0299 / ADR-0314: libvmaf scoring backend. ``None`` (default)
    # omits the ``--backend`` flag so libvmaf picks its own default
    # (CPU on a stock build); ``"cuda"`` / ``"sycl"`` / ``"hip"`` /
    # ``"cpu"`` engage the corresponding backend explicitly. The CLI
    # resolves ``--score-backend auto`` to a concrete value before
    # populating this field; ``CorpusOptions`` itself never walks the
    # fallback chain.
    score_backend: str | None = None
    # HDR mode (Bucket #9, ADR-0300):
    # - "auto": probe each source via ffprobe; inject HDR codec args +
    #   the HDR-VMAF model when PQ / HLG signaling is detected. Default.
    # - "force-sdr": skip detection; treat every source as SDR.
    # - "force-hdr-pq": treat every source as HDR PQ (overrides probe).
    # - "force-hdr-hlg": treat every source as HDR HLG (overrides probe).
    # The active mode lands on each corpus row's ``hdr_mode`` /
    # ``hdr_transfer`` / ``hdr_primaries`` columns.
    hdr_mode: str = "auto"
    ffprobe_bin: str = "ffprobe"
    # Phase F (ADR-0333): opt into 2-pass encoding for codecs whose
    # adapter sets ``supports_two_pass = True`` (libx264 / libx265
    # today; libsvtav1 / libvvenc follow as sibling PRs). Default
    # off — single-pass behaviour stays the canonical path. When set
    # against an adapter where ``supports_two_pass = False``, the
    # encode driver writes a one-line stderr warning and runs
    # single-pass (matching the saliency unsupported-ROI fallback
    # precedent).
    two_pass: bool = False
    # Content-addressed encode cache (ADR-0298): when enabled, iter_rows
    # skips re-encoding cells whose (src_sha256, encoder, preset, crf)
    # key already exists in cache_dir. Default off; CLI enables via
    # --cache-dir. The cache key schema matches vmaftune.cache.
    cache_enabled: bool = False
    cache_dir: Path | None = None
    # Resolution-aware model selection (ADR-0289): when True, iter_rows
    # overrides vmaf_model with the resolution-appropriate model returned
    # by vmaftune.resolution.model_for_resolution — vmaf_4k_v0.6.1 for
    # height >= 2160, vmaf_v0.6.1 otherwise. False keeps the explicit
    # vmaf_model value regardless of source resolution.
    resolution_aware: bool = True


def _encode_path(opts: CorpusOptions, source: Path, preset: str, crf: int) -> Path:
    stem = f"{source.stem}__{opts.encoder}__{preset}__crf{crf}.mp4"
    return opts.encode_dir / stem


def _resolve_sample_clip(job: CorpusJob, opts: CorpusOptions) -> tuple[float, float, int, int, str]:
    """Return ``(clip_seconds, start_s, frame_skip_ref, frame_cnt, clip_mode)``.

    Caps the requested slice at ``job.duration_s`` (so a 10-second
    request against an 8-second source falls back to full-clip rather
    than encoding a short tail). Centre-anchored: ``start_s = (D - N) / 2``.
    Returns the no-op tuple ``(0.0, 0.0, 0, 0, "full")`` when sample-clip
    mode is off or the source is too short.
    """
    requested = float(opts.sample_clip_seconds)
    duration = float(job.duration_s)
    if requested <= 0.0 or duration <= 0.0 or requested >= duration:
        return (0.0, 0.0, 0, 0, "full")
    start_s = max(0.0, (duration - requested) / 2.0)
    # libvmaf CLI takes integer frame counts; framerate may be
    # fractional (e.g. 23.976). Round to nearest to keep the window
    # symmetric around the centre.
    frame_skip_ref = round(start_s * job.framerate)
    frame_cnt = round(requested * job.framerate)
    label = f"sample_{round(requested)}s"
    return (requested, start_s, frame_skip_ref, frame_cnt, label)


_VALID_HDR_MODES = frozenset({"auto", "force-sdr", "force-hdr-pq", "force-hdr-hlg"})


def _synthetic_hdr_info(transfer: str, *, pix_fmt: str) -> HdrInfo:
    """Build an :class:`HdrInfo` for ``--force-hdr-pq`` / ``--force-hdr-hlg``.

    Used when the user knows the source carries the named transfer but
    the container can't surface it (raw YUV refs are the canonical
    case). Mastering-display + max-CLL stay ``None`` — without ffprobe
    we have no way to read them and emitting fabricated values would be
    worse than emitting none.
    """
    forced_pix_fmt = pix_fmt if ("10" in pix_fmt or "12" in pix_fmt) else "yuv420p10le"
    return HdrInfo(
        transfer=transfer,
        primaries="bt2020",
        matrix="bt2020nc",
        color_range="tv",
        pix_fmt=forced_pix_fmt,
    )


def _resolve_hdr(job: CorpusJob, opts: CorpusOptions) -> tuple[HdrInfo | None, bool]:
    """Resolve the effective HDR signaling for ``job`` per ``opts.hdr_mode``.

    Returns ``(info, forced)`` where ``info`` is the HdrInfo to drive
    encoder + scorer wiring (``None`` for SDR), and ``forced`` records
    whether the user overrode auto-detection via one of the
    ``--force-*`` flags. The flag lands on the corpus row's
    ``hdr_forced`` column so Phase B/C consumers can distinguish a
    detected HDR row from a user-asserted one.

    Unknown ``hdr_mode`` values fall back to ``auto`` with a one-shot
    warning — corpus runs shouldn't crash on a typoed CLI flag.
    """
    mode = opts.hdr_mode
    if mode not in _VALID_HDR_MODES:
        _LOG.warning("vmaf-tune: unknown hdr_mode %r; falling back to 'auto'", mode)
        mode = "auto"

    if mode == "force-sdr":
        return (None, True)
    if mode == "force-hdr-pq":
        return (_synthetic_hdr_info("pq", pix_fmt=job.pix_fmt), True)
    if mode == "force-hdr-hlg":
        return (_synthetic_hdr_info("hlg", pix_fmt=job.pix_fmt), True)

    # auto: probe the source via ffprobe. detect_hdr returns None for
    # SDR / probe-failure / missing-binary, all of which are OK — the
    # encode proceeds without HDR signaling.
    info = detect_hdr(job.source, ffprobe_bin=opts.ffprobe_bin)
    return (info, False)


def _resolve_hdr_score_model(
    info: HdrInfo | None,
    sdr_model: str,
    *,
    warned: list[bool],
) -> str:
    """Pick the VMAF model to score against given HDR provenance.

    Returns a string passable to :class:`ScoreRequest.model`. When
    ``info`` is None the SDR model is used unchanged. When ``info`` is
    set we look for an HDR model JSON shipped under ``model/``; if
    none is present we warn once per corpus run and fall back to the
    SDR model — see ADR-0300 (HDR-VMAF model port is a backlog item).
    The ``warned`` list is a one-element mutable flag so the warning
    fires exactly once per ``iter_rows`` invocation rather than per
    cell.
    """
    if info is None:
        return sdr_model
    hdr_model = select_hdr_vmaf_model()
    if hdr_model is not None:
        return f"path={hdr_model}"
    if not warned[0]:
        _LOG.warning(
            "vmaf-tune: HDR source detected (transfer=%s) but no HDR VMAF "
            "model is shipped; scoring against SDR model %r — scores will "
            "trend low for high-luminance regions (see ADR-0300)",
            info.transfer,
            sdr_model,
        )
        warned[0] = True
    return sdr_model


def _resolve_shot_metadata(
    job: CorpusJob,
    *,
    shot_runner: object | None,
    per_shot_bin: str,
) -> ShotMetadata:
    """Run TransNet shot detection once per source and aggregate.

    Failures are silent: ``detect_shots`` already falls back to a
    sentinel list when the binary is missing or the invocation fails;
    :func:`summarise_shots` maps that sentinel to the all-zero
    metadata, which downstream consumers treat as "shot data
    unavailable for this source". The cost of running TransNet is
    paid once per source (not per cell), so we compute it inside
    ``iter_rows`` rather than per-row.
    """
    total_frames = (
        round(job.duration_s * job.framerate)
        if job.duration_s > 0.0 and job.framerate > 0.0
        else None
    )
    shots, ok = _detect_shots_with_status(
        job.source,
        width=job.width,
        height=job.height,
        pix_fmt=job.pix_fmt,
        total_frames=total_frames,
        per_shot_bin=per_shot_bin,
        runner=shot_runner,
    )
    if not ok:
        return ShotMetadata(count=0, avg_duration_sec=0.0, duration_std_sec=0.0)
    return summarise_shots(shots, framerate=job.framerate)


def iter_rows(
    job: CorpusJob,
    opts: CorpusOptions,
    *,
    encode_runner: object | None = None,
    score_runner: object | None = None,
    shot_runner: object | None = None,
    probe_runner: object | None = None,
) -> Iterator[dict[str, Any]]:
    """Yield one JSONL row per (preset, crf) cell.

    ``encode_runner`` / ``score_runner`` / ``shot_runner`` / ``probe_runner``
    are subprocess-runner stubs parameterised for tests. Production
    callers leave them ``None``.
    """
    adapter = get_adapter(opts.encoder)
    src_hash = _sha256_file(job.source) if (opts.src_sha256 and job.source.exists()) else ""

    opts.encode_dir.mkdir(parents=True, exist_ok=True)

    # Content-addressed cache (ADR-0298). Initialise once per iter_rows
    # call so the cache index is loaded only once, not per cell.
    tune_cache = None
    if opts.cache_enabled and opts.cache_dir is not None:
        from .cache import TuneCache

        tune_cache = TuneCache(path=opts.cache_dir)

    clip_seconds, start_s, frame_skip_ref, frame_cnt, clip_mode = _resolve_sample_clip(job, opts)
    shot_meta = _resolve_shot_metadata(job, shot_runner=shot_runner, per_shot_bin="vmaf-perShot")

    # HDR resolution happens once per source: detection (or the forced
    # synthetic info) is constant across the (preset, crf) grid for a
    # given input. Re-probing per cell would burn an ffprobe per encode
    # for no signal gain.
    hdr_info, hdr_forced = _resolve_hdr(job, opts)
    hdr_extra_params: tuple[str, ...] = ()
    if hdr_info is not None:
        hdr_extra_params = hdr_codec_args(opts.encoder, hdr_info)
    score_model_warned = [False]

    # ADR-0499 / BBB e2e v3 Bug #V3-B: decode the *reference* leg to
    # raw YUV once before iterating cells. The libvmaf CLI's
    # ``raw_input_open`` path (active whenever ``--width`` / ``--height``
    # / ``--pixel_format`` / ``--bitdepth`` are passed, which vmaf-tune
    # always does) refuses container/Y4M inputs. Previously only the
    # distorted leg was decoded; container sources tripped the score
    # step with "file size mismatch" and the sampler reported "produced
    # no scorable encodes". Doing this once per ``iter_rows`` call
    # (instead of per cell) keeps the cost flat across CRF/preset
    # sweeps. ``_sp.run`` is used directly: test stubs injected via
    # ``score_runner`` mock the vmaf CLI, not ffmpeg decodes.
    import subprocess as _sp

    # ADR-0501 / BBB e2e v4 Bug #V4-B: when the caller bound source
    # dims distinct from the rung target (cross-resolution ladder)
    # OR when the rung target differs from the raw-YUV source's
    # native geometry, the reference decode must downscale to the
    # rung target so the libvmaf CLI reads both legs at the same
    # width/height. Otherwise the binary mis-parses the planar bytes
    # and emits a catastrophic VMAF (~21 instead of ~93). Container
    # sources without explicit src dims keep the legacy "decode at
    # native geometry" path — width/height for those rungs already
    # match the source after ffmpeg's auto-detect.
    _ref_target_w: int | None = None
    _ref_target_h: int | None = None
    if (
        job.src_width is not None
        and job.src_height is not None
        and ((int(job.src_width), int(job.src_height)) != (int(job.width), int(job.height)))
    ):
        _ref_target_w = int(job.width)
        _ref_target_h = int(job.height)
    # BBB e2e v6 Bug #V6-2 (ADR-0506): pass the source's native
    # geometry into the reference decoder so a raw-YUV source can be
    # parsed by ffmpeg's rawvideo demuxer when a cross-resolution
    # scale is required. For container sources the demuxer auto-
    # detects and the ``source_*`` hints are unused.
    _src_dim_w = int(job.src_width) if job.src_width is not None else int(job.width)
    _src_dim_h = int(job.src_height) if job.src_height is not None else int(job.height)
    decoded_reference, ref_decode_rc = _maybe_decode_reference(
        job.source,
        encode_dir=opts.encode_dir,
        pix_fmt=job.pix_fmt,
        # Cap the reference decode at the analysed window so a 10 s
        # probe doesn't spill ~58 GB of raw YUV (BBB e2e v2 Bug #v2-A).
        duration_s=float(job.duration_s),
        ffmpeg_bin=opts.ffmpeg_bin,
        runner=_sp.run,
        target_width=_ref_target_w,
        target_height=_ref_target_h,
        source_width=_src_dim_w,
        source_height=_src_dim_h,
        source_framerate=float(job.framerate),
    )

    for preset, crf in job.cells:
        adapter.validate(preset, crf)

        # Cache hit: return the cached row without encoding.
        if tune_cache is not None and src_hash:
            from .cache import cache_key

            key = cache_key(
                src_sha256=src_hash,
                encoder=opts.encoder,
                preset=preset,
                crf=crf,
                adapter_version="",
                ffmpeg_version="",
            )
            cached = tune_cache.get(key)
            if cached is not None:
                # Reconstruct a minimal corpus row from CachedResult fields.
                # Provenance metadata (run_id, timestamp) gets a fresh stamp
                # so downstream tools can tell the row came from cache.
                nan = float("nan")
                hit_row: dict[str, Any] = {k: nan for k in CORPUS_ROW_KEYS}
                hit_row.update(
                    {
                        "schema_version": SCHEMA_VERSION,
                        "run_id": uuid.uuid4().hex,
                        "timestamp": _utc_now_iso(),
                        "src": str(job.source),
                        "src_sha256": src_hash,
                        "width": job.width,
                        "height": job.height,
                        "pix_fmt": job.pix_fmt,
                        "framerate": job.framerate,
                        "duration_s": job.duration_s,
                        "encoder": opts.encoder,
                        "encoder_version": cached.encoder_version,
                        "preset": preset,
                        "crf": crf,
                        "extra_params": [],
                        "encode_path": str(cached.artifact_path) if opts.keep_encodes else "",
                        "encode_size_bytes": cached.encode_size_bytes,
                        "bitrate_kbps": bitrate_kbps(
                            cached.encode_size_bytes, job.duration_s or 1.0
                        ),
                        "encode_time_ms": cached.encode_time_ms,
                        "vmaf_score": cached.vmaf_score,
                        "vmaf_model": cached.vmaf_model,
                        "score_time_ms": cached.score_time_ms,
                        "ffmpeg_version": cached.ffmpeg_version,
                        "vmaf_binary_version": cached.vmaf_binary_version,
                        "exit_status": 0,
                        "clip_mode": "full",
                        "hdr_transfer": "",
                        "hdr_primaries": "",
                        "hdr_forced": False,
                        "shot_count": 0,
                        "shot_avg_duration_sec": nan,
                        "shot_duration_std_sec": nan,
                    }
                )
                yield hit_row
                continue

        out = _encode_path(opts, job.source, preset, crf)
        # ADR-0499 / Bug #V3-B: when the once-per-sweep reference
        # decode failed, every cell's score will fail the same way.
        # Synthesize a failed ``EncodeResult`` instead of re-running
        # ffmpeg N times for output we cannot score; the
        # ref-decode-fail branch below short-circuits the score step
        # too. ``_row_for`` requires a non-None ``enc_res`` for the
        # row-shape invariant.
        if ref_decode_rc != 0:
            from .encode import EncodeRequest as _EncReq
            from .encode import EncodeResult as _EncRes

            enc_res = _EncRes(
                request=_EncReq(
                    source=job.source,
                    width=int(job.width),
                    height=int(job.height),
                    pix_fmt=job.pix_fmt,
                    framerate=float(job.framerate),
                    encoder=adapter.encoder,
                    preset=preset,
                    crf=crf,
                    output=out,
                ),
                encode_size_bytes=0,
                encode_time_ms=0.0,
                encoder_version="skipped",
                ffmpeg_version="skipped",
                exit_status=ref_decode_rc,
                stderr_tail=(f"encode skipped: reference decode failed (rc={ref_decode_rc})"),
            )
            base_model = opts.vmaf_model
            score_model = _resolve_hdr_score_model(hdr_info, base_model, warned=score_model_warned)
            score_req = ScoreRequest(
                reference=decoded_reference,
                distorted=out,
                width=job.width,
                height=job.height,
                pix_fmt=job.pix_fmt,
                model=score_model,
                frame_skip_ref=frame_skip_ref,
                frame_cnt=frame_cnt,
                duration_s=float(job.duration_s),
            )
            score_res = ScoreResult(
                request=score_req,
                vmaf_score=float("nan"),
                score_time_ms=0.0,
                vmaf_binary_version="skipped",
                exit_status=ref_decode_rc,
                stderr_tail=(
                    f"reference decode to raw YUV failed (rc={ref_decode_rc}) " f"for {job.source}"
                ),
            )
            row = _row_for(
                job=job,
                opts=opts,
                preset=preset,
                crf=crf,
                src_sha=src_hash,
                enc_res=enc_res,
                score_res=score_res,
                score_model=score_model,
                clip_mode=clip_mode,
                hdr_info=hdr_info,
                hdr_forced=hdr_forced,
                shot_meta=shot_meta,
            )
            yield row
            continue
        # ADR-0498 / Bug #v2-B: when the caller supplied source dims
        # distinct from the rung target, tell ffmpeg the *source*
        # geometry on the input side (-s) and add a -vf scale=W:H
        # filter so the encoded rendition lands at the rung target.
        # Both fields ``None`` keeps the legacy behaviour where the
        # rung target serves as both source and encode geometry.
        enc_src_w = int(job.src_width) if job.src_width is not None else int(job.width)
        enc_src_h = int(job.src_height) if job.src_height is not None else int(job.height)
        # ADR-0505 / BBB e2e v5 Bug #V5-2 root cause: when the source is
        # a container/Y4M (anything outside :data:`_VMAF_RAW_SUFFIXES`)
        # the encode pipe MUST treat it as a container — letting ffmpeg
        # auto-detect format and resolution. The historic path always
        # built the encode argv with ``-f rawvideo -pix_fmt … -s WxH``,
        # which reinterprets the container's compressed bytes as planar
        # YUV pixels and produces a catastrophic encode (uniformly
        # ~50 Mbps regardless of CRF, garbage frames, VMAF in the 4-9
        # band). The reference leg is already decoded to raw YUV by
        # :func:`_maybe_decode_reference`; the encode leg gets the
        # complementary fix here so cross-resolution scoring is well-
        # defined and the CRF flag actually controls bitrate.
        source_is_container = job.source.suffix.lower() not in _VMAF_RAW_SUFFIXES
        scale_extra: tuple[str, ...] = ()
        if source_is_container:
            # Container sources: enforce the rung target via a scale
            # filter unconditionally (ffmpeg's auto-detected geometry
            # may not match the requested rendition). For native-
            # geometry rungs the scale is a cheap no-op.
            scale_extra = ("-vf", f"scale={int(job.width)}:{int(job.height)}")
        elif (enc_src_w, enc_src_h) != (int(job.width), int(job.height)):
            scale_extra = ("-vf", f"scale={int(job.width)}:{int(job.height)}")
        enc_req = EncodeRequest(
            source=job.source,
            width=enc_src_w,
            height=enc_src_h,
            pix_fmt=job.pix_fmt,
            framerate=job.framerate,
            encoder=adapter.encoder,
            preset=preset,
            crf=crf,
            output=out,
            extra_params=tuple(hdr_extra_params) + scale_extra,
            sample_clip_seconds=clip_seconds,
            sample_clip_start_s=start_s,
            source_is_container=source_is_container,
            # BBB e2e v6 Bug #V6-1 (ADR-0506): plumb the job's analysed
            # window length so the encode is bounded when the caller
            # didn't opt into sample-clip mode (the ladder/CLI
            # ``--duration`` flag exercises this path). The reference
            # decode already honours ``job.duration_s``; mirroring it on
            # the encode side stops a 10-second smoke run from re-
            # encoding the full 9-minute source.
            duration_s=float(job.duration_s),
        )
        if opts.two_pass:
            # Phase F (ADR-0333). The driver gracefully falls back
            # to single-pass when the adapter does not opt into
            # 2-pass; keeps mixed-codec corpora honest.
            enc_res = run_two_pass_encode(
                enc_req,
                ffmpeg_bin=opts.ffmpeg_bin,
                runner=encode_runner,
            )
        elif getattr(adapter, "supports_encoder_stats", False):
            # ADR-0332: codec adapters that emit a parseable pass-1 stats
            # file opt in via ``supports_encoder_stats``; the dispatcher
            # routes those through the stats-capturing wrapper. Hardware
            # encoders fall through to the legacy single-pass path.
            enc_res = run_encode_with_stats(
                enc_req,
                ffmpeg_bin=opts.ffmpeg_bin,
                runner=encode_runner,
            )
        else:
            enc_res = run_encode(enc_req, ffmpeg_bin=opts.ffmpeg_bin, runner=encode_runner)

        base_model = opts.vmaf_model
        if opts.resolution_aware:
            from .resolution import select_vmaf_model_version

            base_model = select_vmaf_model_version(job.width, job.height)
        score_model = _resolve_hdr_score_model(hdr_info, base_model, warned=score_model_warned)
        score_req = ScoreRequest(
            # ADR-0499 / Bug #V3-B: ``decoded_reference`` is the
            # pre-decoded raw-YUV path when ``job.source`` was a
            # container, or ``job.source`` itself when the source was
            # already raw. Container sources that fail to decode are
            # short-circuited above (the cell yields a failed row
            # without invoking ffmpeg or the vmaf binary).
            reference=decoded_reference,
            distorted=out,
            width=job.width,
            height=job.height,
            pix_fmt=job.pix_fmt,
            model=score_model,
            frame_skip_ref=frame_skip_ref,
            frame_cnt=frame_cnt,
            # BBB e2e v2 Bug #v2-A: forward the job duration so the
            # post-encode container -> raw YUV decode is bounded.
            duration_s=float(job.duration_s),
        )
        if enc_res.exit_status == 0:
            # The vmaf CLI only reads raw .yuv / .y4m input; decode the
            # encoded container to a temporary YUV before scoring.
            # Always use the real subprocess for the decode step — test
            # stubs injected via ``score_runner`` handle vmaf CLI calls only.
            import subprocess as _sp

            score_req = _maybe_decode_distorted(
                score_req,
                encode_dir=opts.encode_dir,
                ffmpeg_bin=opts.ffmpeg_bin,
                runner=_sp.run,
            )
            score_res = run_score(
                score_req,
                vmaf_bin=opts.vmaf_bin,
                runner=score_runner,
                backend=opts.score_backend,
            )
        else:
            # Skip scoring on encode failure; row records the failure.
            score_res = ScoreResult(
                request=score_req,
                vmaf_score=float("nan"),
                score_time_ms=0.0,
                vmaf_binary_version="skipped",
                exit_status=enc_res.exit_status,
                stderr_tail="encode failed; score skipped",
            )

        row = _row_for(
            job=job,
            opts=opts,
            preset=preset,
            crf=crf,
            src_sha=src_hash,
            enc_res=enc_res,
            score_res=score_res,
            score_model=score_model,
            clip_mode=clip_mode,
            hdr_info=hdr_info,
            hdr_forced=hdr_forced,
            shot_meta=shot_meta,
        )
        # Cache put: must happen BEFORE cleanup so artifact_path exists.
        # Store successful rows so the next run gets a hit.
        if tune_cache is not None and src_hash and enc_res.exit_status == 0:
            from .cache import CachedResult, TuneCache, cache_key

            key = cache_key(
                src_sha256=src_hash,
                encoder=opts.encoder,
                preset=preset,
                crf=crf,
                adapter_version="",
                ffmpeg_version=enc_res.ffmpeg_version,
            )
            with contextlib.suppress(Exception):
                # Re-compute key with the same placeholder values used at
                # lookup so get(key) and put(key, ...) are always
                # consistent. adapter_version / ffmpeg_version are set
                # to "" in both paths; the corpus row records the real
                # values in the encoder_version / ffmpeg_version columns.
                put_key = cache_key(
                    src_sha256=src_hash,
                    encoder=opts.encoder,
                    preset=preset,
                    crf=crf,
                    adapter_version="",
                    ffmpeg_version="",
                )
                tune_cache.put(
                    put_key,
                    CachedResult(
                        encode_size_bytes=enc_res.encode_size_bytes,
                        encode_time_ms=enc_res.encode_time_ms,
                        encoder_version=enc_res.encoder_version,
                        ffmpeg_version=enc_res.ffmpeg_version,
                        vmaf_score=score_res.vmaf_score,
                        vmaf_model=score_model,
                        score_time_ms=score_res.score_time_ms,
                        vmaf_binary_version=score_res.vmaf_binary_version,
                        artifact_path=out,  # placeholder; put() overwrites it
                    ),
                    artifact_path=out,
                )

        if not opts.keep_encodes and out.exists() and enc_res.exit_status == 0:
            # best-effort cleanup; corpus row stays valid either way
            with contextlib.suppress(OSError):
                out.unlink()

        yield row


def _row_for(
    *,
    job: CorpusJob,
    opts: CorpusOptions,
    preset: str,
    crf: int,
    src_sha: str,
    enc_res,
    score_res,
    score_model: str = "",
    clip_mode: str = "full",
    hdr_info: HdrInfo | None = None,
    hdr_forced: bool = False,
    shot_meta: ShotMetadata | None = None,
) -> dict:
    # Bitrate is computed against the *encoded* duration so sample-clip
    # rows aren't biased low by dividing slice-bytes by full-source
    # seconds. ``duration_s`` keeps the source provenance.
    encoded_duration_s = (
        enc_res.request.sample_clip_seconds
        if enc_res.request.sample_clip_seconds > 0.0
        else job.duration_s
    )
    row = {
        "schema_version": SCHEMA_VERSION,
        "run_id": uuid.uuid4().hex,
        "timestamp": _utc_now_iso(),
        "src": str(job.source),
        "src_sha256": src_sha,
        "width": job.width,
        "height": job.height,
        "pix_fmt": job.pix_fmt,
        "framerate": job.framerate,
        "duration_s": job.duration_s,
        "encoder": opts.encoder,
        "encoder_version": enc_res.encoder_version,
        "preset": preset,
        "crf": crf,
        "extra_params": list(enc_res.request.extra_params),
        "encode_path": (str(enc_res.request.output) if opts.keep_encodes else ""),
        "encode_size_bytes": enc_res.encode_size_bytes,
        "bitrate_kbps": bitrate_kbps(enc_res.encode_size_bytes, encoded_duration_s),
        "encode_time_ms": enc_res.encode_time_ms,
        "vmaf_score": score_res.vmaf_score,
        "vmaf_model": score_model,
        "score_time_ms": score_res.score_time_ms,
        "ffmpeg_version": enc_res.ffmpeg_version,
        "vmaf_binary_version": score_res.vmaf_binary_version,
        "exit_status": enc_res.exit_status or score_res.exit_status,
        "clip_mode": clip_mode,
        "hdr_transfer": hdr_info.transfer if hdr_info is not None else "",
        "hdr_primaries": hdr_info.primaries if hdr_info is not None else "",
        "hdr_forced": bool(hdr_forced),
        # TransNet-V2 shot-metadata trio (research-0086). When shot
        # detection is unavailable the fields are zero-valued so
        # downstream loaders can filter on ``shot_count > 0`` without
        # special-casing missing keys.
        "shot_count": (shot_meta.count if shot_meta is not None else 0),
        "shot_avg_duration_sec": (shot_meta.avg_duration_sec if shot_meta is not None else 0.0),
        "shot_duration_std_sec": (shot_meta.duration_std_sec if shot_meta is not None else 0.0),
    }
    # v3 canonical-6 aggregate columns (ADR-0366). Missing features
    # (model didn't expose them, or scoring was skipped) become NaN —
    # callers may filter on isnan() / pandas .dropna() rather than
    # train on synthetic zeros. Iteration is in canonical order so
    # downstream positional consumers stay stable.
    feature_means = score_res.feature_means or {}
    feature_stds = score_res.feature_stds or {}
    for feature, mean_key, std_key in zip(
        CANONICAL6_FEATURES, CANONICAL6_MEAN_KEYS, CANONICAL6_STD_KEYS, strict=True
    ):
        row[mean_key] = float(feature_means.get(feature, float("nan")))
        row[std_key] = float(feature_stds.get(feature, float("nan")))
    # ADR-0332: encoder-internal stats aggregates. Always emit the
    # ten ``enc_internal_*`` columns so v3 rows are schema-uniform
    # across codecs; aggregator returns zeros for empty input.
    encoder_stats_frames = getattr(enc_res, "encoder_stats", ())
    row.update(aggregate_stats(encoder_stats_frames))
    # Schema-shape assertion — catches drift in development; cheap.
    missing = set(CORPUS_ROW_KEYS) - row.keys()
    if missing:
        raise AssertionError(f"corpus row missing keys: {sorted(missing)}")
    return row


def write_jsonl(rows: Sequence[dict] | Iterator[dict], path: Path) -> int:
    """Write ``rows`` to ``path`` (one JSON object per line). Returns count."""
    path.parent.mkdir(parents=True, exist_ok=True)
    n = 0
    with path.open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row, sort_keys=True))
            fh.write(os.linesep)
            n += 1
    return n


def read_jsonl(path: Path, *, upgrade_to_current: bool = True) -> list[dict]:
    """Read a corpus JSONL with forward / backward schema compatibility.

    Rows whose ``schema_version <= 2`` predate the canonical-6 column
    addition (ADR-0366). When ``upgrade_to_current`` is True (the
    default), the reader fills the missing v3 columns with ``NaN`` so
    downstream consumers can treat every row as v3-shaped without
    crashing on ``KeyError``. ``schema_version`` itself is *not*
    rewritten — the original value is preserved so callers that want
    to filter ``< 3`` (e.g. trainers requiring real per-feature data)
    can do so.

    Pass ``upgrade_to_current=False`` to get the bare on-disk row.
    """
    rows: list[dict] = []
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            if upgrade_to_current:
                _upgrade_row_in_place(row)
            rows.append(row)
    return rows


def _upgrade_row_in_place(row: dict) -> None:
    """Fill missing v3 canonical-6 columns with NaN on legacy rows."""
    sv = row.get("schema_version")
    try:
        sv_int = int(sv) if sv is not None else 0
    except (TypeError, ValueError):
        sv_int = 0
    if sv_int >= SCHEMA_VERSION:
        # Even if the row is current, defensively backfill any missing
        # column (a partial write would otherwise crash positional code).
        for key in (*CANONICAL6_MEAN_KEYS, *CANONICAL6_STD_KEYS):
            row.setdefault(key, float("nan"))
        return
    for key in (*CANONICAL6_MEAN_KEYS, *CANONICAL6_STD_KEYS):
        row.setdefault(key, float("nan"))


# ---------------------------------------------------------------------------
# Coarse-to-fine search (ADR-0306)
# ---------------------------------------------------------------------------
#
# Full-grid sweep over CRF 0..51 step 1 = 52 encodes per (source, preset).
# When the caller only wants "smallest CRF that meets a VMAF target" we can
# bracket in two passes:
#
#   1. Coarse pass at every ``coarse_step`` over the CRF range.
#   2. Fine pass at step ``fine_step`` within ``±fine_radius`` of the
#      best-coarse point.
#
# Defaults (10/5/1 over 0..51) produce 5 + 10 = 15 unique encodes — a 3.5x
# wall-time speedup vs the full grid with no measurable quality loss on the
# Netflix Public corpus (see docs/research/0067 + ADR-0296).
#
# When no ``target_vmaf`` is supplied, the orchestrator still runs both
# passes and refines around the highest-VMAF coarse point.


def _crf_clamp(crf: int) -> int:
    """Clamp a CRF candidate to the libx264 0..51 valid range."""
    if crf < 0:
        return 0
    if crf > 51:
        return 51
    return crf


def coarse_grid_crfs(
    *,
    crf_min: int = 10,
    crf_max: int = 50,
    coarse_step: int = 10,
) -> tuple[int, ...]:
    """Return the coarse-pass CRF grid as a deduped, sorted tuple.

    Defaults yield ``(10, 20, 30, 40, 50)`` — 5 points spanning the
    practically useful CRF range for libx264. CRF below 10 is visually
    lossless on most content (huge bitrate, no perceptual gain) and
    CRF=51 is the codec floor; the coarse pass intentionally skips
    both. Override ``crf_min`` / ``crf_max`` for codecs with different
    quality-knob ranges.
    """
    if coarse_step <= 0:
        raise ValueError(f"coarse_step must be positive, got {coarse_step}")
    if crf_min > crf_max:
        raise ValueError(f"crf_min ({crf_min}) > crf_max ({crf_max})")
    n = math.floor((crf_max - crf_min) / coarse_step) + 1
    grid = sorted({_crf_clamp(crf_min + i * coarse_step) for i in range(n)})
    return tuple(grid)


def fine_grid_crfs(
    best_crf: int,
    *,
    fine_radius: int = 5,
    fine_step: int = 1,
    crf_min: int = 0,
    crf_max: int = 51,
    exclude: Sequence[int] = (),
) -> tuple[int, ...]:
    """Return CRF candidates in ``[best - radius, best + radius]`` at ``fine_step``.

    Cells in ``exclude`` (typically the coarse-pass grid) are removed so the
    second pass only visits points that haven't been measured already.
    """
    if fine_radius < 0:
        raise ValueError(f"fine_radius must be non-negative, got {fine_radius}")
    if fine_step <= 0:
        raise ValueError(f"fine_step must be positive, got {fine_step}")
    excluded = set(exclude)
    candidates: set[int] = set()
    for delta in range(-fine_radius, fine_radius + 1, fine_step):
        candidates.add(_crf_clamp(best_crf + delta))
    candidates.difference_update(excluded)
    # Keep candidates inside the configured range.
    candidates = {c for c in candidates if crf_min <= c <= crf_max}
    return tuple(sorted(candidates))


def _pick_best_crf(
    rows: Sequence[dict],
    *,
    target_vmaf: float | None,
) -> int | None:
    """Identify the "best" coarse CRF for refinement.

    With a target: highest CRF whose ``vmaf_score`` meets ``target_vmaf``.
    That's the smallest-quality candidate that still passes the gate, so
    refining around it locates the smallest acceptable CRF.

    Without a target: the CRF with the highest VMAF (lowest CRF in
    practice, but tie-broken by score). NaN / failed rows are ignored.
    """

    def _score(row: dict) -> float:
        v = row.get("vmaf_score")
        try:
            v = float(v)
        except (TypeError, ValueError):
            return float("nan")
        return v

    valid = [r for r in rows if not math.isnan(_score(r))]
    if not valid:
        return None

    if target_vmaf is None:
        winner = max(valid, key=_score)
        return int(winner["crf"])

    passing = [r for r in valid if _score(r) >= target_vmaf]
    if passing:
        # Highest CRF that still passes — refining around it finds the
        # smallest CRF that still meets the target.
        winner = max(passing, key=lambda r: int(r["crf"]))
        return int(winner["crf"])
    # Nothing met the target on the coarse pass. Fall back to the
    # highest-VMAF coarse point so the fine pass at least probes near
    # the achievable ceiling.
    winner = max(valid, key=_score)
    return int(winner["crf"])


def _should_skip_refinement(
    *,
    best_crf: int | None,
    coarse_grid: Sequence[int],
    target_vmaf: float | None,
    best_score: float,
    crf_max: int,
) -> bool:
    """Decide whether the coarse pass alone is enough.

    The fine pass is skipped when:

    - the coarse pass produced no measurable rows (best_crf is None), or
    - a target is set, the best-coarse CRF *meets* the target, and
      refining higher cannot help (the best-coarse is already at the
      highest CRF in the coarse grid OR pinned at ``crf_max``). In that
      case there are no larger CRF candidates to probe, so the existing
      best already minimises bitrate at the gate.
    """
    if best_crf is None:
        return True
    if target_vmaf is None:
        return False
    if math.isnan(best_score):
        return False
    if best_score < target_vmaf:
        return False
    # Target met — refining to the right would only check higher CRFs
    # (lower quality). We can skip if we're already at the max-CRF
    # coarse cell or pinned at crf_max.
    return best_crf >= max(coarse_grid) or best_crf >= crf_max


def coarse_to_fine_search(
    job: CorpusJob,
    opts: CorpusOptions,
    *,
    target_vmaf: float | None = None,
    coarse_step: int = 10,
    fine_radius: int = 5,
    fine_step: int = 1,
    crf_min: int = 10,
    crf_max: int = 50,
    encode_runner: object | None = None,
    score_runner: object | None = None,
) -> Iterator[dict]:
    """Run a 2-pass coarse-to-fine CRF search.

    Yields the same JSONL rows :func:`iter_rows` does — coarse pass
    first, then fine pass (if not skipped). The caller is responsible
    for selecting the chosen CRF from the rows; this function only
    drives the encodes.

    The presets in ``job.cells`` are honoured: the search runs once
    per distinct preset, with the CRF axis replaced by the
    coarse-then-fine sweep.
    """
    presets = tuple(dict.fromkeys(p for p, _crf in job.cells))
    if not presets:
        return

    coarse_grid = coarse_grid_crfs(crf_min=crf_min, crf_max=crf_max, coarse_step=coarse_step)

    for preset in presets:
        coarse_cells = tuple((preset, c) for c in coarse_grid)
        coarse_job = dataclasses.replace(job, cells=coarse_cells)
        coarse_rows: list[dict] = []
        for row in iter_rows(
            coarse_job,
            opts,
            encode_runner=encode_runner,
            score_runner=score_runner,
        ):
            coarse_rows.append(row)
            yield row

        best_crf = _pick_best_crf(coarse_rows, target_vmaf=target_vmaf)
        best_score = float("nan")
        if best_crf is not None:
            for r in coarse_rows:
                if int(r["crf"]) == best_crf:
                    try:
                        best_score = float(r["vmaf_score"])
                    except (TypeError, ValueError):
                        best_score = float("nan")
                    break

        if _should_skip_refinement(
            best_crf=best_crf,
            coarse_grid=coarse_grid,
            target_vmaf=target_vmaf,
            best_score=best_score,
            crf_max=crf_max,
        ):
            continue

        # mypy: best_crf cannot be None here — _should_skip_refinement
        # would have returned True above.
        assert best_crf is not None
        fine_crfs = fine_grid_crfs(
            best_crf,
            fine_radius=fine_radius,
            fine_step=fine_step,
            crf_min=crf_min,
            crf_max=crf_max,
            exclude=coarse_grid,
        )
        if not fine_crfs:
            continue

        fine_cells = tuple((preset, c) for c in fine_crfs)
        fine_job = dataclasses.replace(job, cells=fine_cells)
        for row in iter_rows(
            fine_job,
            opts,
            encode_runner=encode_runner,
            score_runner=score_runner,
        ):
            yield row
