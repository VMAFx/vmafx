# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Phase E — per-title bitrate-ladder generator.

Given a single source clip, sample the (resolution, quality) plane,
compute the convex hull of the resulting (bitrate, vmaf) points (the
Pareto frontier), pick a small number of "knee" renditions along the
hull, and emit an ABR ladder descriptor (HLS master playlist, DASH MPD,
or JSON).

This mirrors the Netflix per-title encoding paper's central idea: the
optimal ladder for one title is *not* a fixed grid — it is the set of
(resolution, bitrate) points that maximise quality per byte for *that*
title.

Production sampling is wired by composing Phase A's
:func:`vmaftune.corpus.iter_rows` (encode + score) with the
:func:`vmaftune.recommend.pick_target_vmaf` predicate from Phase B's
recommend surface (ADR-0306 / Research-0079). The :data:`SamplerFn`
seam stays open so callers can substitute a finer grid, a Bayesian
bisect, or a precomputed corpus row stream.

See ``docs/adr/0295-vmaf-tune-phase-e-bitrate-ladder.md`` for the
design rationale and the alternatives considered (geometric ladder,
fixed Apple HLS authoring spec, JND-based, etc.) and
``docs/adr/0307-vmaf-tune-ladder-default-sampler.md`` for the
default-sampler wiring decision.

Uncertainty-aware rung selection (ADR-0279, this PR)
-----------------------------------------------------

The conformal-VQA prediction surface (PR #488) attaches a
``(low, high)`` interval to every predicted VMAF point. When the
sampler ships those intervals on the :class:`UncertaintyLadderPoint`
extension, two new transforms become available:

* :func:`prune_redundant_rungs_by_uncertainty` — drop adjacent rungs
  whose conformal intervals overlap by more than a configurable
  fraction of the wider rung's width. Rationale: when rung A's
  ``[low_A, high_A]`` and rung B's ``[low_B, high_B]`` overlap on
  more than ``overlap_threshold`` (default ``0.5`` per Research-0067)
  the predictor cannot statistically distinguish the two rungs at
  the nominal coverage level, so the lower-bitrate rung is
  redundant — keep the higher-quality one.
* :func:`insert_extra_rungs_in_high_uncertainty_regions` — for any
  pair of adjacent rungs whose averaged interval width is above the
  ``wide_interval_min_width`` gate (default ``5.0`` VMAF), insert a
  synthetic mid-bitrate / mid-quality rung. Rationale: a wide
  interval is exactly where ladder choices have the most empirical
  impact (the predictor can't tell which of "ship rung A" vs "ship
  a hypothetical mid-rung" is better), so probing the midpoint is
  the highest-information-per-encode use of the budget.

Both transforms are *post-hull* — they run after
:func:`convex_hull` and before :func:`select_knees` so the
Pareto-frontier invariant is preserved. The default sampler preserves
a corpus row's ``vmaf_interval`` block. When callers opt in to
uncertainty but only provide point estimates, the recipe uses the
active wide-interval threshold as a conservative centred fallback so
point-only rows still probe uncertain gaps instead of silently
skipping the ladder recipe.

Per :mod:`vmaftune.uncertainty` documentation, the uncertainty
recipe **only** changes which rungs the ladder builder evaluates;
it does **not** widen the production-flip gate.
"""

from __future__ import annotations

import dataclasses
import itertools
import json
import math
import tempfile
from collections.abc import Callable, Iterable, Sequence
from pathlib import Path
from typing import cast

from .defaultmodel import DEFAULT_MODEL
from .uncertainty import ConfidenceDecision, ConfidenceThresholds, classify_interval

# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class LadderPoint:
    """One sampled (resolution, bitrate, vmaf, crf) point.

    Produced by :func:`build_ladder` for every (resolution, target_vmaf)
    request. ``crf`` is the encoder quality knob the bisect converged
    on; downstream encodes can re-use it directly.
    """

    width: int
    height: int
    bitrate_kbps: float
    vmaf: float
    crf: int

    @property
    def pixel_count(self) -> int:
        return self.width * self.height


@dataclasses.dataclass(frozen=True)
class Rendition:
    """One rung of the final ABR ladder."""

    width: int
    height: int
    bitrate_kbps: float
    vmaf: float
    crf: int


@dataclasses.dataclass(frozen=True)
class Ladder:
    """Result of :func:`build_ladder` — the raw sampled grid.

    The convex hull and rendition picks are derived later by
    :func:`convex_hull` and :func:`select_knees`.
    """

    src: Path
    encoder: str
    points: tuple[LadderPoint, ...]


# ---------------------------------------------------------------------------
# Sampling — produces the (resolution, bitrate, vmaf) cloud
# ---------------------------------------------------------------------------


# Type alias for the corpus-generator callback. In production this wraps
# Phase B's target-VMAF bisect; in tests it returns synthetic points.
SamplerFn = Callable[[Path, str, int, int, float], LadderPoint]


def build_ladder(
    src: Path,
    encoder: str,
    resolutions: Sequence[tuple[int, int]],
    target_vmafs: Sequence[float],
    *,
    sampler: SamplerFn | None = None,
) -> Ladder:
    """Sample the (resolution x target_vmaf) plane for one source.

    For each (resolution, target_vmaf) cell, ``sampler`` produces a
    :class:`LadderPoint`. Production callers leave ``sampler`` ``None``
    to dispatch to :func:`_default_sampler`, which composes the Phase A
    corpus encode+score loop with :func:`recommend.pick_target_vmaf` to
    pick the (preset_default, CRF) row whose VMAF is closest to
    ``target_vmaf`` over the canonical 5-point CRF sweep
    ``(18, 23, 28, 33, 38)`` (ADR-0307, Research-0079). Tests inject a
    stub via ``sampler=`` to avoid live encoder runs.
    """
    if sampler is None:
        sampler = _default_sampler

    points: list[LadderPoint] = []
    for w, h in resolutions:
        for tv in target_vmafs:
            pt = sampler(src, encoder, w, h, tv)
            points.append(pt)
    return Ladder(src=src, encoder=encoder, points=tuple(points))


# Canonical 5-point CRF sweep used by the default sampler (ADR-0307).
# Spans the perceptually-informative range across every shipped adapter;
# each non-x264 adapter validates the points against its own
# ``quality_range`` inside ``corpus.iter_rows``. Callers needing a finer
# grid pass an explicit ``sampler=`` to ``build_ladder``.
#
# Bug N-2 (regression covered by ``tests/test_ladder_svtav1_default_crf.py``):
# the earlier ``(18, 23, 28, 33, 38)`` sweep started at CRF 18, which is
# below the libsvtav1 Phase A lower bound (``SvtAv1Adapter.quality_range
# = (20, 50)``).  ``adapter.validate(preset, 18)`` raised ``ValueError``
# and the ladder exited 2 before any encode could run.  The current
# starting point is 20 — the maximum of every shipped adapter's lower
# bound — so the sweep is valid for **every** adapter without needing
# an explicit ``--crf-sweep`` override.  Step size is preserved at 5 so
# the sweep still spans 20 CRF points of operating-region coverage.
DEFAULT_SAMPLER_CRF_SWEEP: tuple[int, ...] = (20, 25, 30, 35, 40)


def _default_sampler_preset(encoder: str) -> str:
    """Pick the codec adapter's mid-range preset for the default sweep.

    Most adapters expose ``"medium"`` in their ``presets`` tuple; the
    fallback walks the tuple and returns its midpoint name.
    """
    # Lazy import — keeps the corpus / codec_adapters dependency off
    # the import path for callers that only use ``convex_hull`` /
    # ``select_knees`` / ``emit_manifest``.
    from .codec_adapters import get_adapter

    adapter = get_adapter(encoder)
    presets = tuple(adapter.presets)
    if "medium" in presets:
        return "medium"
    if not presets:
        raise ValueError(f"adapter {encoder!r} declares no presets")
    return presets[len(presets) // 2]


def make_default_sampler(
    *,
    pix_fmt: str = "yuv420p",
    framerate: float = 24.0,
    duration_s: float = 1.0,
    crf_sweep: Sequence[int] | None = None,
    src_width: int | None = None,
    src_height: int | None = None,
    cloud_sink: list[LadderPoint] | None = None,
    score_backend: str | None = None,
    vmaf_model: str = DEFAULT_MODEL,
) -> SamplerFn:
    """Return a :data:`SamplerFn` closed over real source-shape metadata.

    The legacy module-level :func:`_default_sampler` hardcoded
    ``framerate=24.0``, ``duration_s=1.0``, ``pix_fmt="yuv420p"`` and
    the canonical 5-point CRF sweep. The CLI had no way to override
    any of them — a 1080p30 / 60 s source therefore ran the corpus
    sweep at 24 fps / 1 s, producing nonsense bitrate math
    (kbps = file_size / 1.0) and timing out on real content (Bug #4
    and #5, BBB e2e 2026-05-17). This factory closes over the actual
    source shape (resolved by the CLI from ``--framerate`` /
    ``--duration`` / ``--pix-fmt`` flags or ffprobe) plus an optional
    CRF sweep override (``--crf-sweep``) so smoke runs can pick a
    short sweep instead of the production 5-point grid.

    ``src_width`` / ``src_height`` (added 2026-05-18, ADR-0498) carry
    the actual source resolution separately from the per-rung target
    resolution. When the source is raw YUV at one resolution and the
    ladder requests a different rendition, the sampler injects an
    ffmpeg ``scale`` filter on the encode pipe and tells ffmpeg the
    true source dimensions on the input side — the historic path used
    the target dims as the input ``-s`` argument, which produces
    frame-corruption on every rung where target != source (BBB e2e v2
    Bug #v2-B). When left at ``None`` the legacy behaviour is
    preserved (target dims serve as both source and encode dims).

    ``score_backend`` (added 2026-05-18, Bug C / ADR-0509) threads the
    ``--score-backend`` CLI value into every corpus call the default
    sampler makes. ``None`` keeps the existing auto-select behaviour
    (libvmaf picks the fastest available backend).

    ``vmaf_model`` (added ADR-0622) threads the ``--vmaf-model``
    (and its NEG variant when ``--neg`` is set) into every corpus call.
    The default preserves the historic ``vmaf_v0.6.1`` behaviour for
    callers that do not pass ``vmaf_model``.
    """
    sweep = tuple(int(c) for c in crf_sweep) if crf_sweep is not None else DEFAULT_SAMPLER_CRF_SWEEP

    def _sampler(
        src: Path, encoder: str, width: int, height: int, target_vmaf: float
    ) -> LadderPoint:
        return _default_sampler(
            src,
            encoder,
            width,
            height,
            target_vmaf,
            pix_fmt=pix_fmt,
            framerate=framerate,
            duration_s=duration_s,
            crf_sweep=sweep,
            src_width=src_width,
            src_height=src_height,
            cloud_sink=cloud_sink,
            score_backend=score_backend,
            vmaf_model=vmaf_model,
        )

    return _sampler


def _default_sampler(
    src: Path,
    encoder: str,
    width: int,
    height: int,
    target_vmaf: float,
    *,
    pix_fmt: str = "yuv420p",
    framerate: float = 24.0,
    duration_s: float = 1.0,
    crf_sweep: Sequence[int] | None = None,
    src_width: int | None = None,
    src_height: int | None = None,
    cloud_sink: list[LadderPoint] | None = None,
    score_backend: str | None = None,
    vmaf_model: str = DEFAULT_MODEL,
) -> LadderPoint:
    """Production sampler — encode the configured CRF sweep, pick by VMAF.

    Composes :func:`vmaftune.corpus.iter_rows` (Phase A encode+score)
    with :func:`vmaftune.recommend.pick_target_vmaf` (Phase B-equivalent
    smallest-CRF-meeting-target predicate). The JSONL corpus is
    written to a tempfile that's discarded after the call returns; the
    encode-side temp dir lives under the same prefix and is cleaned up
    on exit.

    Defaults remain the historical placeholders (``yuv420p`` /
    24 fps / 1 s) for back-compat with callers that pass this function
    by name as ``sampler=``; the production CLI now binds the real
    source shape via :func:`make_default_sampler` so bitrate math and
    encode framerate match the input.

    BBB e2e v2 Bug #v2-B: when ``src_width`` / ``src_height`` are set
    and differ from the rung's ``(width, height)``, the corpus job is
    configured with the *actual* source dimensions and an extra
    ``-vf scale=W:H`` filter is appended via ``CorpusOptions.extra_args``
    so ffmpeg decodes the source at its native geometry and scales to
    the requested rendition. The historic single-resolution code path
    (``src_width`` / ``src_height`` left at ``None``) is preserved.
    """
    # Lazy imports — see ``_default_sampler_preset``.
    from .corpus import CorpusJob, CorpusOptions, iter_rows
    from .recommend import pick_target_vmaf

    preset = _default_sampler_preset(encoder)
    sweep = tuple(int(c) for c in crf_sweep) if crf_sweep is not None else DEFAULT_SAMPLER_CRF_SWEEP
    cells = tuple((preset, crf) for crf in sweep)

    # Resolve the dims to hand the corpus job:
    # - When the caller bound ``src_width / src_height`` (CLI path, ADR-0498)
    #   AND they differ from the rung target, decode at the source geometry
    #   and add a scale filter to the encode pipe.
    # - Otherwise (caller left them None, or source == target) the legacy
    #   single-resolution path is taken — corpus job receives the target dims.
    use_src_dims = (
        src_width is not None
        and src_height is not None
        and (src_width, src_height) != (width, height)
    )

    with tempfile.TemporaryDirectory(prefix="vmaftune-ladder-") as tmp:
        tmp_path = Path(tmp)
        # ``CorpusJob.{width,height}`` are the rung's *target* dimensions
        # used by the libvmaf score step and (when no source dims are
        # supplied) by the encoder's raw-YUV input shape. ``src_width``
        # / ``src_height`` (added 2026-05-18, ADR-0498) carry the actual
        # source dimensions so :func:`vmaftune.corpus.iter_rows` can
        # tell ffmpeg the demuxer geometry separately from the encoded
        # rendition geometry and inject a ``-vf scale=W:H`` filter when
        # they differ — fixes Bug #v2-B (the historic code path passed
        # the rung target as the source ``-s`` argument, corrupting
        # every cross-res rung against a raw-YUV source).
        job = CorpusJob(
            source=src,
            width=int(width),
            height=int(height),
            pix_fmt=pix_fmt,
            framerate=float(framerate),
            duration_s=float(duration_s),
            cells=cells,
            src_width=int(src_width) if use_src_dims else None,
            src_height=int(src_height) if use_src_dims else None,
        )
        opts = CorpusOptions(
            encoder=encoder,
            output=tmp_path / "corpus.jsonl",
            encode_dir=tmp_path / "encodes",
            keep_encodes=False,
            src_sha256=False,
            score_backend=score_backend,
            vmaf_model=vmaf_model,
        )
        rows = [r for r in iter_rows(job, opts) if int(r.get("exit_status", 0)) == 0]

    if not rows:
        raise RuntimeError(
            f"default sampler produced no scorable encodes for "
            f"{src} at {width}x{height} (encoder={encoder}); pass an "
            f"explicit sampler= to build_ladder() to debug."
        )

    # ADR-0505 / BBB e2e v5 Bug #V5-2: when a cloud sink is wired in,
    # capture every successfully-scored CRF row before the
    # ``pick_target_vmaf`` collapse. Downstream JSON ``samples`` then
    # contains the full per-CRF cloud per resolution instead of one
    # winner per (resolution, target_vmaf) cell. The sink is shared
    # across cells in the same ``build_and_emit`` call, so the
    # ``_dedup_samples`` pass on the emit side keeps the array
    # unique by ``(width, height, crf)``.
    if cloud_sink is not None:
        for row in rows:
            cloud_sink.append(_ladder_point_from_row(width, height, row))

    pick = pick_target_vmaf(rows, target_vmaf)
    return _ladder_point_from_row(width, height, pick.row)


def _ladder_point_from_row(width: int, height: int, row: dict) -> LadderPoint:
    """Build a ladder point from a corpus row, preserving intervals if present.

    When the row carries a ``vmaf_interval`` (conformal CV+ pipeline) the
    runtime return is an :class:`UncertaintyLadderPoint`, which is *not*
    a :class:`LadderPoint` subclass (see that class's docstring for the
    rationale). The annotation is kept as ``LadderPoint`` because the
    downstream :class:`Ladder.points` tuple is typed that way and
    callers structurally consume only the LadderPoint-shaped fields;
    the broader union would cascade through the public ladder API. The
    cast tells the type-checker about the controlled widening.
    """
    base = {
        "width": width,
        "height": height,
        "bitrate_kbps": float(row["bitrate_kbps"]),
        "vmaf": float(row["vmaf_score"]),
        "crf": int(row["crf"]),
    }
    interval = row.get("vmaf_interval")
    if isinstance(interval, dict) and "low" in interval and "high" in interval:
        return cast(
            "LadderPoint",
            UncertaintyLadderPoint(
                **base,
                vmaf_low=float(interval["low"]),
                vmaf_high=float(interval["high"]),
            ),
        )
    return LadderPoint(**base)


# ---------------------------------------------------------------------------
# Convex hull — Pareto frontier on (bitrate, vmaf)
# ---------------------------------------------------------------------------


def convex_hull(points: Iterable[LadderPoint]) -> list[LadderPoint]:
    """Upper-convex hull of the (bitrate, vmaf) cloud (Pareto frontier).

    Two passes:

    1. **Pareto filter** — drop any point dominated by another (lower
       or equal bitrate AND higher or equal vmaf, with one inequality
       strict). This already gives the staircase frontier.
    2. **Upper-convex hull** — over the remaining monotonically
       rising staircase, drop interior points whose surrounding
       neighbours form a *concave* arc above them (keep only the
       points on the *convex* envelope so an ABR algorithm switching
       between them sees diminishing returns, never increasing).

    On the resulting hull, both bitrate and vmaf are strictly
    monotonic; no other rendition strictly dominates any hull point.
    """
    pts = list(points)
    if not pts:
        return []
    # Sort by bitrate ascending, vmaf descending so duplicate-bitrate
    # clusters keep the higher-quality first.
    pts.sort(key=lambda p: (p.bitrate_kbps, -p.vmaf))

    # Pareto filter: walk left-to-right tracking the running max vmaf;
    # only emit points that strictly raise it.
    pareto: list[LadderPoint] = []
    best_vmaf = -math.inf
    last_bitrate: float | None = None
    for p in pts:
        if last_bitrate is not None and p.bitrate_kbps == last_bitrate:
            continue  # already kept higher-vmaf duplicate
        if p.vmaf > best_vmaf:
            pareto.append(p)
            best_vmaf = p.vmaf
            last_bitrate = p.bitrate_kbps

    if len(pareto) <= 2:
        return pareto

    # Upper-convex hull on the staircase. We want a *concave*
    # envelope on the (bitrate, vmaf) plane (diminishing returns):
    # walking left-to-right, the slope must be non-increasing. Pop
    # the previous point while it would create a non-concave angle
    # (cross product >= 0 means the new point is collinear or above
    # the line from -2 to -1, so -1 is redundant).
    hull: list[LadderPoint] = []
    for p in pareto:
        while len(hull) >= 2 and _cross(hull[-2], hull[-1], p) >= 0:
            hull.pop()
        hull.append(p)
    return hull


def _cross(o: LadderPoint, a: LadderPoint, b: LadderPoint) -> float:
    """2D cross product of vectors (o->a) x (o->b).

    Positive = counter-clockwise turn (point ``b`` is above the line
    o->a, which is what we want on the upper hull).
    """
    return (a.bitrate_kbps - o.bitrate_kbps) * (b.vmaf - o.vmaf) - (a.vmaf - o.vmaf) * (
        b.bitrate_kbps - o.bitrate_kbps
    )


# ---------------------------------------------------------------------------
# Knee selection — pick `n` renditions along the hull
# ---------------------------------------------------------------------------


def select_knees(
    hull: Sequence[LadderPoint],
    n: int = 5,
    *,
    spacing: str = "log_bitrate",
) -> list[Rendition]:
    """Pick ``n`` rungs along the Pareto hull.

    ``spacing`` controls the parameter the rungs are evenly spaced in:

    - ``"log_bitrate"`` — Apple HLS authoring spec convention; rungs
      double in bitrate as you go up the ladder. Default.
    - ``"vmaf"`` — perceptually-spaced; equal VMAF gap per rung.
      ``"uniform"`` is accepted as a legacy alias for this mode.

    The first and last hull points are always included, and the
    interior rungs are picked by snapping the ideal coordinate to the
    nearest hull point. Result is sorted ascending by bitrate.
    """
    if not hull:
        return []
    if n <= 0:
        raise ValueError("n must be >= 1")
    if n == 1:
        # Pick the highest-quality point — most useful single rung.
        top = max(hull, key=lambda p: p.vmaf)
        return [_rendition_of(top)]
    if len(hull) <= n:
        return [_rendition_of(p) for p in hull]

    targets = _ideal_targets(hull, n, spacing)
    chosen: list[LadderPoint] = []
    seen: set[int] = set()
    for t in targets:
        idx = _nearest_index(hull, t, spacing)
        # Avoid duplicates if two targets snap to the same point.
        while idx in seen and idx + 1 < len(hull):
            idx += 1
        seen.add(idx)
        chosen.append(hull[idx])
    chosen.sort(key=lambda p: p.bitrate_kbps)
    return [_rendition_of(p) for p in chosen]


def _ideal_targets(hull: Sequence[LadderPoint], n: int, spacing: str) -> list[float]:
    if spacing == "uniform":
        spacing = "vmaf"
    if spacing == "log_bitrate":
        lo = math.log(max(hull[0].bitrate_kbps, 1.0))
        hi = math.log(max(hull[-1].bitrate_kbps, 1.0))
    elif spacing == "vmaf":
        lo = min(p.vmaf for p in hull)
        hi = max(p.vmaf for p in hull)
    else:
        raise ValueError(f"unknown spacing: {spacing!r}")
    if hi <= lo:
        return [lo] * n
    step = (hi - lo) / (n - 1)
    return [lo + i * step for i in range(n)]


def _nearest_index(hull: Sequence[LadderPoint], target: float, spacing: str) -> int:
    if spacing == "uniform":
        spacing = "vmaf"

    def _key(p: LadderPoint) -> float:
        if spacing == "log_bitrate":
            return math.log(max(p.bitrate_kbps, 1.0))
        return p.vmaf

    best_i = 0
    best_d = abs(_key(hull[0]) - target)
    for i in range(1, len(hull)):
        d = abs(_key(hull[i]) - target)
        if d < best_d:
            best_i = i
            best_d = d
    return best_i


def _rendition_of(p: LadderPoint) -> Rendition:
    return Rendition(
        width=p.width,
        height=p.height,
        bitrate_kbps=p.bitrate_kbps,
        vmaf=p.vmaf,
        crf=p.crf,
    )


# ---------------------------------------------------------------------------
# Manifest emit — HLS / DASH / JSON
# ---------------------------------------------------------------------------


def emit_manifest(
    ladder: Sequence[Rendition],
    format: str = "hls",
    *,
    samples: Sequence[LadderPoint] | None = None,
) -> str:
    """Serialise a list of :class:`Rendition` rungs in the requested format.

    Supported formats:

    - ``"hls"`` — Apple HLS master playlist with one
      ``#EXT-X-STREAM-INF`` per rung. Bandwidth is reported in bps
      (HLS spec); resolution as ``WxH``. Variant URIs are placeholders
      (``rendition_<W>x<H>_<kbps>k.m3u8``); the consumer re-points
      them at real per-rendition playlists.
    - ``"dash"`` — DASH MPD with one ``Representation`` per rung
      under a single ``AdaptationSet``. Minimal but spec-conformant.
    - ``"json"`` — JSON descriptor (the canonical machine-readable
      form for downstream tooling). When ``samples`` is provided the
      JSON payload also carries a top-level ``samples`` array — every
      raw ``(resolution, target_vmaf)`` cell that the sampler scored,
      pre-hull. ``vmaf-tune report`` reads the array to render a hull
      plot; downstream diff tooling reads it to compare the sampled
      cloud across runs. Non-JSON formats ignore ``samples``.

    Output is a string; callers write to disk if needed. Renditions
    are emitted in ascending-bitrate order.
    """
    sorted_ladder = sorted(ladder, key=lambda r: r.bitrate_kbps)
    if format == "hls":
        return _emit_hls(sorted_ladder)
    if format == "dash":
        return _emit_dash(sorted_ladder)
    if format == "json":
        return _emit_json(sorted_ladder, samples=samples)
    raise ValueError(f"unknown manifest format: {format!r} (expected hls/dash/json)")


def _emit_hls(ladder: Sequence[Rendition]) -> str:
    lines: list[str] = ["#EXTM3U", "#EXT-X-VERSION:6"]
    for r in ladder:
        bps = round(r.bitrate_kbps * 1000.0)
        uri = f"rendition_{r.width}x{r.height}_{round(r.bitrate_kbps)}k.m3u8"
        lines.append(
            f"#EXT-X-STREAM-INF:BANDWIDTH={bps},RESOLUTION={r.width}x{r.height},"
            f'CODECS="avc1.640028"'
        )
        lines.append(uri)
    return "\n".join(lines) + "\n"


def _emit_dash(ladder: Sequence[Rendition]) -> str:
    reps: list[str] = []
    for i, r in enumerate(ladder):
        bps = round(r.bitrate_kbps * 1000.0)
        reps.append(
            f'    <Representation id="r{i}" bandwidth="{bps}" '
            f'width="{r.width}" height="{r.height}" '
            f'codecs="avc1.640028" mimeType="video/mp4">\n'
            f"      <BaseURL>rendition_{r.width}x{r.height}_"
            f"{round(r.bitrate_kbps)}k.mp4</BaseURL>\n"
            f"    </Representation>"
        )
    body = "\n".join(reps)
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" '
        'type="static" minBufferTime="PT2S" '
        'profiles="urn:mpeg:dash:profile:isoff-on-demand:2011">\n'
        "  <Period>\n"
        '    <AdaptationSet contentType="video" segmentAlignment="true">\n'
        f"{body}\n"
        "    </AdaptationSet>\n"
        "  </Period>\n"
        "</MPD>\n"
    )


def _emit_json(ladder: Sequence[Rendition], *, samples: Sequence[LadderPoint] | None = None) -> str:
    payload: dict[str, object] = {
        "schema": "vmaf-tune-ladder/v1",
        "renditions": [
            {
                "width": r.width,
                "height": r.height,
                "bitrate_kbps": r.bitrate_kbps,
                "bandwidth_bps": round(r.bitrate_kbps * 1000.0),
                "vmaf": r.vmaf,
                "crf": r.crf,
            }
            for r in ladder
        ],
    }
    # ADR-0501 / BBB e2e v4 Bug #V4-B: always emit a ``samples`` array
    # (possibly empty when the caller never wired the pre-hull cloud
    # through). Downstream consumers (``vmaf-tune report``) read it to
    # plot the Pareto frontier overlay; an absent key forced them into
    # the legacy ``points`` fallback or to count ``ladder_samples=0``.
    samples_list = list(samples) if samples is not None else []
    payload["samples"] = [
        {
            "width": p.width,
            "height": p.height,
            "bitrate_kbps": float(p.bitrate_kbps),
            "bandwidth_bps": round(float(p.bitrate_kbps) * 1000.0),
            "vmaf": float(p.vmaf),
            "crf": int(p.crf),
        }
        for p in sorted(samples_list, key=lambda x: (x.width * x.height, x.bitrate_kbps))
    ]
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"


# ---------------------------------------------------------------------------
# Top-level convenience — build + hull + select + emit in one call
# ---------------------------------------------------------------------------


def build_and_emit(
    src: Path,
    encoder: str,
    resolutions: Sequence[tuple[int, int]],
    target_vmafs: Sequence[float],
    *,
    quality_tiers: int = 5,
    format: str = "hls",
    spacing: str = "log_bitrate",
    sampler: SamplerFn | None = None,
    with_uncertainty: bool = False,
    uncertainty_thresholds: ConfidenceThresholds | None = None,
    rung_overlap_threshold: float | None = None,
    point_interval_width: float | None = None,
    extra_samples: Sequence[LadderPoint] | None = None,
) -> str:
    """Convenience: build → hull → select → emit, returns the manifest string.

    ADR-0505 / BBB e2e v5 Bug #V5-2 + #V5-3: when ``extra_samples`` is
    provided it supersedes the per-target ``ladder.points`` cloud as
    the source of the JSON ``samples`` array. The historic path emitted
    one sample per ``(resolution, target_vmaf)`` cell — for a sweep
    with multiple CRF candidates per resolution this dropped every
    non-winning CRF row and double-listed duplicates whenever two
    target-VMAFs picked the same CRF. ``extra_samples`` lets the
    caller pass the *full* per-CRF cloud produced by the sampler so
    downstream consumers (``vmaf-tune report`` Pareto overlay,
    diff tooling) see every scored point exactly once.

    Samples are de-duplicated by ``(width, height, crf)`` before
    emission so the JSON descriptor is stable across sampler quirks
    (e.g. two targets that select the same CRF still yield one
    sample row, not two).
    """
    ladder = build_ladder(src, encoder, resolutions, target_vmafs, sampler=sampler)
    hull = convex_hull([_plain_ladder_point(p) for p in ladder.points])
    if with_uncertainty:
        thresholds = uncertainty_thresholds or ConfidenceThresholds()
        if point_interval_width is None:
            point_interval_width = thresholds.wide_interval_min_width
        uncertainty_hull = _restore_uncertainty_on_hull(
            hull,
            ladder.points,
            point_interval_width=point_interval_width,
        )
        overlap = (
            DEFAULT_RUNG_OVERLAP_THRESHOLD
            if rung_overlap_threshold is None
            else rung_overlap_threshold
        )
        adjusted = apply_uncertainty_recipe(
            uncertainty_hull,
            thresholds=thresholds,
            overlap_threshold=overlap,
        )
        hull = [p.as_ladder_point() for p in adjusted]
    rungs = select_knees(hull, n=quality_tiers, spacing=spacing)
    # ADR-0501 / BBB e2e v4 Bug #V4-B: thread the pre-hull sample
    # cloud through so the JSON emitter's ``samples`` array carries
    # every scored cell.
    # ADR-0505 / BBB e2e v5 Bug #V5-2 + #V5-3: prefer ``extra_samples``
    # (the full per-CRF sweep, dedup'd) over the per-target picks so
    # report consumers see every encoded point, not the V4-B subset
    # that double-listed targets which selected the same CRF.
    if extra_samples is not None:
        plain_samples = [_plain_ladder_point(p) for p in extra_samples]
    else:
        plain_samples = [_plain_ladder_point(p) for p in ladder.points]
    plain_samples = _dedup_samples(plain_samples)
    return emit_manifest(rungs, format=format, samples=plain_samples)


def _dedup_samples(samples: Sequence[LadderPoint]) -> list[LadderPoint]:
    """Drop ``(width, height, crf)`` duplicates from a sample cloud.

    Stable: the first occurrence wins so the caller's ordering on the
    surviving rows is preserved. Added 2026-05-18 for ADR-0505 to fix
    the V5-3 double-append symptom that surfaced in the v4 emit path
    (two target-VMAFs picking the same CRF would emit two identical
    sample rows).
    """
    seen: set[tuple[int, int, int]] = set()
    out: list[LadderPoint] = []
    for p in samples:
        key = (int(p.width), int(p.height), int(p.crf))
        if key in seen:
            continue
        seen.add(key)
        out.append(p)
    return out


# ---------------------------------------------------------------------------
# Uncertainty-aware rung selection (ADR-0279, PR #488 wiring)
# ---------------------------------------------------------------------------


# Default per-pair overlap fraction above which adjacent rungs are
# treated as statistically indistinguishable. ``0.5`` is the
# conservative midpoint floor documented in Research-0067 §"Phase F
# decision tree" — at 50 % overlap on the wider rung's interval, the
# probability of rung B's true VMAF lying in rung A's interval is
# already non-trivial, so the marginal information gained by
# shipping both rungs falls below the cost of the extra encode.
DEFAULT_RUNG_OVERLAP_THRESHOLD: float = 0.5


@dataclasses.dataclass(frozen=True)
class UncertaintyLadderPoint:
    """:class:`LadderPoint` augmented with a conformal interval.

    ``vmaf_low`` / ``vmaf_high`` carry the conformal lower / upper
    bounds at the calibration's nominal coverage level (typically
    95 %, alpha=0.05). ``vmaf == point`` from the underlying
    predictor; the interval is centred on it but need not be
    symmetric (the CV+ form is non-symmetric).

    Subclassing :class:`LadderPoint` would require runtime
    isinstance gymnastics in the existing transforms; instead we
    expose :meth:`as_ladder_point` so the uncertainty-aware
    pipeline can convert back to the plain shape before handing
    off to :func:`convex_hull` / :func:`select_knees`.
    """

    width: int
    height: int
    bitrate_kbps: float
    vmaf: float
    crf: int
    vmaf_low: float
    vmaf_high: float

    @property
    def interval_width(self) -> float:
        """Conformal interval width (>= 0)."""
        return max(0.0, float(self.vmaf_high) - float(self.vmaf_low))

    def as_ladder_point(self) -> LadderPoint:
        """Project to the plain :class:`LadderPoint` shape."""
        return LadderPoint(
            width=self.width,
            height=self.height,
            bitrate_kbps=self.bitrate_kbps,
            vmaf=self.vmaf,
            crf=self.crf,
        )


def _plain_ladder_point(point: LadderPoint | UncertaintyLadderPoint) -> LadderPoint:
    if isinstance(point, UncertaintyLadderPoint):
        return point.as_ladder_point()
    return point


def _point_key(
    point: LadderPoint | UncertaintyLadderPoint,
) -> tuple[int, int, float, float, int]:
    return (
        point.width,
        point.height,
        float(point.bitrate_kbps),
        float(point.vmaf),
        int(point.crf),
    )


def _restore_uncertainty_on_hull(
    hull: Sequence[LadderPoint],
    sampled_points: Sequence[LadderPoint | UncertaintyLadderPoint],
    *,
    point_interval_width: float | None = None,
) -> list[UncertaintyLadderPoint]:
    """Attach sampled intervals to hull points; point-only rows get a centred fallback."""
    intervals = {
        _point_key(point): point
        for point in sampled_points
        if isinstance(point, UncertaintyLadderPoint)
    }
    fallback_width = 0.0 if point_interval_width is None else max(0.0, float(point_interval_width))
    fallback_half_width = 0.5 * fallback_width
    restored: list[UncertaintyLadderPoint] = []
    for point in hull:
        interval = intervals.get(_point_key(point))
        if isinstance(interval, UncertaintyLadderPoint):
            restored.append(interval)
            continue
        restored.append(
            UncertaintyLadderPoint(
                width=point.width,
                height=point.height,
                bitrate_kbps=point.bitrate_kbps,
                vmaf=point.vmaf,
                crf=point.crf,
                vmaf_low=point.vmaf - fallback_half_width,
                vmaf_high=point.vmaf + fallback_half_width,
            )
        )
    return restored


def _interval_overlap_fraction(a: UncertaintyLadderPoint, b: UncertaintyLadderPoint) -> float:
    """Overlap of intervals ``a`` and ``b`` over the wider interval's width.

    Returns ``0.0`` if the intervals are disjoint, ``1.0`` if one
    interval fully contains the other. Uses the *wider* width as the
    denominator so the metric is symmetric in ``(a, b)`` and so a
    pinhole-narrow interval inside a wide interval scores ``1.0``
    (the wide interval cannot localise the narrow one's centre).
    """
    overlap_low = max(a.vmaf_low, b.vmaf_low)
    overlap_high = min(a.vmaf_high, b.vmaf_high)
    overlap = max(0.0, overlap_high - overlap_low)
    denom = max(a.interval_width, b.interval_width)
    if denom <= 0.0:
        return 0.0
    return overlap / denom


def prune_redundant_rungs_by_uncertainty(
    rungs: Sequence[UncertaintyLadderPoint],
    *,
    overlap_threshold: float = DEFAULT_RUNG_OVERLAP_THRESHOLD,
) -> list[UncertaintyLadderPoint]:
    """Drop adjacent rungs whose conformal intervals overlap too much.

    Walks the input in ascending-bitrate order and, for every
    adjacent pair ``(prev, cur)`` whose overlap fraction (per
    :func:`_interval_overlap_fraction`) is greater than
    ``overlap_threshold``, drops ``prev`` — the lower-bitrate rung.
    Rationale: when the predictor cannot statistically distinguish
    rungs A and B, ship the higher-quality one (B) and drop A; the
    operator pays one encode budget instead of two for
    indistinguishable quality.

    The first and last rungs are always retained so the hull's
    bitrate range is preserved (``select_knees`` later picks
    interior rungs from whatever remains).

    Returns the filtered list. Input list is not mutated. When
    ``len(rungs) <= 2`` the input is returned verbatim — there is
    no interior to prune.
    """
    if not 0.0 <= overlap_threshold <= 1.0:
        raise ValueError(f"overlap_threshold must be in [0, 1]; got {overlap_threshold!r}")
    if len(rungs) <= 2:
        return list(rungs)
    sorted_rungs = sorted(rungs, key=lambda p: p.bitrate_kbps)
    kept: list[UncertaintyLadderPoint] = [sorted_rungs[0]]
    n = len(sorted_rungs)
    for i, cur in enumerate(sorted_rungs[1:], start=1):
        is_last = i == n - 1
        prev = kept[-1]
        overlap = _interval_overlap_fraction(prev, cur)
        if overlap > overlap_threshold and not is_last:
            # Drop ``prev`` — keep the higher-quality rung instead.
            # The first rung is a special case (always retained), so
            # only swap if ``prev`` isn't the anchor.
            if len(kept) > 1:
                kept[-1] = cur
            else:
                # ``prev`` is the anchor; keep both so the bitrate
                # range stays anchored at the low end.
                kept.append(cur)
        else:
            kept.append(cur)
    return kept


def insert_extra_rungs_in_high_uncertainty_regions(
    rungs: Sequence[UncertaintyLadderPoint],
    *,
    thresholds: ConfidenceThresholds | None = None,
) -> list[UncertaintyLadderPoint]:
    """Insert mid-bitrate rungs where the predictor is uncertain.

    For each adjacent pair ``(a, b)`` in the input, classifies the
    pair-averaged interval width via
    :func:`vmaftune.uncertainty.classify_interval`. When the
    averaged width is in the :attr:`ConfidenceDecision.WIDE` band
    (>= ``wide_interval_min_width``, default ``5.0`` VMAF), insert
    a synthetic rung at the geometric midpoint of the bitrate axis
    and the arithmetic midpoint of the VMAF axis. The synthetic
    rung's interval is set to the union of the parent intervals so
    the recipe is conservative (subsequent encodes refine it).

    The ``crf`` of the synthetic rung is the rounded average of the
    parent rungs' CRFs, the resolution is inherited from the
    higher-quality parent (matching the per-resolution semantics of
    :class:`Rendition`).

    Returns a new list with the synthetic rungs interleaved in
    ascending-bitrate order. Input is not mutated. No-op when
    ``len(rungs) < 2``.
    """
    if thresholds is None:
        thresholds = ConfidenceThresholds()
    if len(rungs) < 2:
        return list(rungs)
    sorted_rungs = sorted(rungs, key=lambda p: p.bitrate_kbps)
    out: list[UncertaintyLadderPoint] = []
    for a, b in itertools.pairwise(sorted_rungs):
        out.append(a)
        avg_width = 0.5 * (a.interval_width + b.interval_width)
        if classify_interval(avg_width, thresholds) is ConfidenceDecision.WIDE:
            mid_bitrate = math.sqrt(max(a.bitrate_kbps, 1e-9) * max(b.bitrate_kbps, 1e-9))
            mid_vmaf = 0.5 * (a.vmaf + b.vmaf)
            mid_low = min(a.vmaf_low, b.vmaf_low)
            mid_high = max(a.vmaf_high, b.vmaf_high)
            mid_crf = round(0.5 * (a.crf + b.crf))
            mid_w = b.width if b.vmaf >= a.vmaf else a.width
            mid_h = b.height if b.vmaf >= a.vmaf else a.height
            out.append(
                UncertaintyLadderPoint(
                    width=mid_w,
                    height=mid_h,
                    bitrate_kbps=mid_bitrate,
                    vmaf=mid_vmaf,
                    crf=mid_crf,
                    vmaf_low=mid_low,
                    vmaf_high=mid_high,
                )
            )
    out.append(sorted_rungs[-1])
    return out


def apply_uncertainty_recipe(
    rungs: Sequence[UncertaintyLadderPoint],
    *,
    thresholds: ConfidenceThresholds | None = None,
    overlap_threshold: float = DEFAULT_RUNG_OVERLAP_THRESHOLD,
) -> list[UncertaintyLadderPoint]:
    """Compose the prune + insert transforms in their canonical order.

    Pruning runs first so the inserted mid-rungs aren't immediately
    re-pruned against their parents (which would defeat the
    information-gain motivation). The composed transform is the
    canonical entry point downstream callers use:

    1. Drop adjacent rungs whose intervals overlap too much.
    2. Insert mid-rungs into any remaining wide-interval gaps.

    Returns a new list. Input is not mutated.
    """
    pruned = prune_redundant_rungs_by_uncertainty(rungs, overlap_threshold=overlap_threshold)
    return insert_extra_rungs_in_high_uncertainty_regions(pruned, thresholds=thresholds)


__all__ = [
    "DEFAULT_RUNG_OVERLAP_THRESHOLD",
    "DEFAULT_SAMPLER_CRF_SWEEP",
    "Ladder",
    "LadderPoint",
    "Rendition",
    "SamplerFn",
    "UncertaintyLadderPoint",
    "apply_uncertainty_recipe",
    "build_and_emit",
    "build_ladder",
    "convex_hull",
    "emit_manifest",
    "insert_extra_rungs_in_high_uncertainty_regions",
    "make_default_sampler",
    "prune_redundant_rungs_by_uncertainty",
    "select_knees",
]
