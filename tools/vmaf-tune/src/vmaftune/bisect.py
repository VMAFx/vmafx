# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Phase B — target-VMAF bisect.

Given a (source, codec, target VMAF) triple, find the *largest* CRF
whose actual measured VMAF still meets the target. "Largest" because
higher CRF = lower bitrate at acceptable quality — that's the cost-
optimal point on the CRF axis.

The algorithm is the obvious one (matches the analytical-curve binary
search in :func:`vmaftune.predictor.pick_crf` but operates on real
encodes via the existing :mod:`vmaftune.encode` / :mod:`vmaftune.score`
seams):

1. Encode at the midpoint CRF of the current ``[lo, hi]`` window and
   score with libvmaf.
2. If measured VMAF >= target, the window narrows upward
   (try a higher CRF — we can compress harder).
3. Else the window narrows downward (we need higher quality).
4. Stop when the window collapses to a single CRF or after
   ``max_iterations``.

The midpoint rounds toward the **lower-quality** end of the window so
we never accept a CRF whose VMAF we have not actually measured: a
clean off-by-one safety net for the "best so far" record.

The bisect assumes monotone-decreasing VMAF in CRF for the (codec,
content) under test. Adjacent samples that violate this contract are
flagged via ``error`` rather than silently accepted; we never
fall back to a different search strategy because the AGENTS-pinned
invariant is "bisect requires monotonicity, hard error otherwise"
(see ``tools/vmaf-tune/AGENTS.md`` Phase B section). Real-world content
is monotone in CRF for every modern codec; pathological cases are
ours-to-fix in the encoder, not ours-to-paper-over here.

Subprocess boundary is the test seam: ``encode_runner`` and
``score_runner`` mirror the pattern from ``encode.run_encode`` /
``score.run_score`` so unit tests inject deterministic stubs.

Phase B is the production wiring the existing ``compare`` /
``recommend-saliency`` / ``predict`` / ``tune-per-shot`` / ``ladder``
subcommands have been stubbing out via the
``NotImplementedError("Phase B pending")`` placeholder predicate.
"""

from __future__ import annotations

import contextlib
import dataclasses
import logging
import math
import os
import shutil
import tempfile
import threading
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .codec_adapters import get_adapter
from .codec_adapters import registry as encoder_registry
from .defaultmodel import DEFAULT_MODEL
from .encode import EncodeRequest, bitrate_kbps, run_encode
from .score import VMAF_RAW_SUFFIXES, ScoreRequest, maybe_decode_distorted, run_score

if TYPE_CHECKING:
    from .compare import PredicateFn, RecommendResult
    from .score_backend import NRProxyBackend

_log = logging.getLogger(__name__)


# ADR-0577: module-level decode semaphore, initialised to 1 by default
# (serial decodes). The CLI / bisect entry point replaces this with a
# Semaphore(N) when the operator passes ``--max-concurrent-decodes N > 1``.
# Using a module-level sentinel lets the same bisect module serve both the
# CLI path (which owns the semaphore lifetime) and unit tests (which can
# swap in a fake semaphore).
_decode_semaphore: threading.Semaphore = threading.Semaphore(1)

# Default value exposed so the CLI can display it in --help and tests can
# reset the global to the canonical default.
DEFAULT_MAX_CONCURRENT_DECODES: int = 1


def set_decode_semaphore(max_concurrent: int) -> None:
    """Replace the module-level decode semaphore with a new one.

    Call this once at startup (e.g. from the CLI ``_run_compare`` family)
    before spawning the thread pool. Thread-safe: the assignment is atomic
    in CPython. Callers that want the default (serial) behaviour do not
    need to call this.

    Args:
        max_concurrent: Maximum concurrent reference-YUV decode operations.
            ``1`` = serial decodes (safest, default). Higher values trade
            peak disk space for throughput on operators with large volumes.
    """
    global _decode_semaphore
    if max_concurrent < 1:
        raise ValueError(f"max_concurrent must be >= 1, got {max_concurrent}")
    _decode_semaphore = threading.Semaphore(max_concurrent)


# Sentinel: a measured VMAF below this floor against a non-degenerate
# encode signals a sample failure, not a real low-quality result. We
# refuse to draw a monotonicity conclusion from such samples.
_VMAF_VALID_FLOOR: float = 0.0
_VMAF_VALID_CEIL: float = 100.0


# ADR-0538 — the bisect search window when the caller passes
# ``crf_range=None`` is the encoder's absolute quality range: the bounds
# the encoder accepts at the FFmpeg CLI, NOT the perceptually-informative
# window adapters expose via :attr:`CodecAdapter.quality_range`. The
# premium-archival defaults (``--target-vmafs 94,96,97,98``) frequently
# require CRFs below the informative window — e.g. libsvtav1's
# ``quality_range = (20, 50)`` is too tight to ever reach VMAF 97.
#
# The absolute ranges live in the encoder registry (ADR-3388,
# ``api/vmafx-encoders.toml`` ``absolute_range``), shared with the Go
# ``pkg/encoder``; sources are in
# docs/research/2026-05-18-premium-vmaf-bisect.md. Hardware encoders and
# VVenC carry an absolute range equal to their quality window there,
# since their CQ / QP scales refuse CRF 0 by design.


def _workdir_parent() -> Path | None:
    """Return the preferred parent directory for temporary work directories.

    Resolution order (ADR-0598):

    1. ``VMAFTUNE_WORKDIR`` environment variable (set by the dev-mcp
       container to ``/probes/vmaftune-work`` which has ~435 GB free),
       *provided the path is writable by the current process*.  When
       ``/probes`` is bind-mounted read-only (e.g. the container user
       uid=2000 has not been granted write access), the env-var path is
       silently skipped and the fallback applies.
    2. ``None`` — callers fall back to the OS default (usually
       ``/tmp``, an 8 GB tmpfs inside the dev-mcp container — too
       small for a full 1080p60 YUV decode of BBB, but better than a
       ``PermissionError``).

    Returns a :class:`Path` when the env var is set *and writable*
    (the directory is created on demand by the caller), otherwise
    ``None``.
    """
    env_val = os.environ.get("VMAFTUNE_WORKDIR", "").strip()
    if not env_val:
        return None
    candidate = Path(env_val)
    # Attempt to create the directory so the writability probe below
    # works even when the path does not yet exist.
    try:
        candidate.mkdir(parents=True, exist_ok=True)
    except OSError:
        pass
    if os.access(candidate, os.W_OK):
        return candidate
    import logging

    logging.getLogger(__name__).warning(
        "VMAFTUNE_WORKDIR=%s is not writable (uid=%d); falling back to OS default temp directory",
        env_val,
        os.getuid(),
    )
    return None


# Size in bytes of a single pixel in each supported pix_fmt.
# yuv420p  → 1.5 bytes/px  (Y plane full + U + V half)
# yuv420p10le → 3 bytes/px (same layout but 16-bit / plane)
# yuv422p  → 2 bytes/px
# yuv444p  → 3 bytes/px
_BYTES_PER_PIXEL: dict[str, float] = {
    "yuv420p": 1.5,
    "yuv420p10le": 3.0,
    "yuv420p12le": 3.0,
    "yuv422p": 2.0,
    "yuv422p10le": 4.0,
    "yuv444p": 3.0,
    "yuv444p10le": 6.0,
}
_BYTES_PER_PIXEL_DEFAULT: float = 1.5  # safe floor for unknown formats


def _midrun_disk_headroom(src: Path) -> float:
    """Return decode headroom for a bisect iteration.

    Container sources need room for a reference decode plus the distorted
    decode. Pre-decoded/raw sources already *are* the reference, so the
    iteration needs only the distorted decode plus normal file overhead.
    """
    return 2.0 if src.suffix.lower() not in VMAF_RAW_SUFFIXES else 1.1


def _estimate_yuv_bytes(
    *,
    width: int,
    height: int,
    pix_fmt: str,
    fps: float,
    duration_s: float,
) -> int:
    """Estimate the disk bytes a raw YUV decode will occupy.

    Used for the preflight disk-space check (ADR-0598). The estimate
    is intentionally rounded up — we multiply by the ceiling of fps and
    add a small per-frame overhead for alignment, so the check is
    conservative rather than optimistic.
    """
    bpp = _BYTES_PER_PIXEL.get(pix_fmt, _BYTES_PER_PIXEL_DEFAULT)
    frames = max(1, math.ceil(fps * max(duration_s, 0.0)))
    return math.ceil(width * height * bpp * frames)


def _check_disk_space(
    workdir: Path,
    *,
    estimated_bytes: int,
    headroom: float = 1.1,
    context: str = "",
) -> str | None:
    """Return an error string if ``workdir``'s volume lacks disk space.

    Compares ``shutil.disk_usage(workdir).free`` against
    ``estimated_bytes * headroom``. Returns ``None`` when space is
    sufficient. Returns a human-readable diagnostic (including GB
    figures and a ``--workdir`` hint) when space is insufficient.

    The check is best-effort: if ``disk_usage`` raises (e.g. on an
    exotic filesystem) the function returns ``None`` (allow the decode
    to proceed) rather than blocking legitimate runs.

    Args:
        workdir: Path whose volume to check.
        estimated_bytes: Raw YUV size estimate in bytes.
        headroom: Multiplier applied to ``estimated_bytes`` before
            comparing against free space. ``2.0`` means the volume
            must have 2× the estimated decode size free — the
            conservative mid-run default (ADR-0577) that accommodates
            the encoded MKV + the decoded YUV coexisting on disk.
        context: Optional codec/target context string for mid-run
            diagnostics (e.g. ``"libx264 @ VMAF 96"``). Appended to
            the error message when non-empty.
    """
    required = math.ceil(estimated_bytes * headroom)
    try:
        usage = shutil.disk_usage(workdir)
        free = usage.free
    except OSError:
        return None  # cannot query — let the decode attempt proceed
    if free >= required:
        return None
    est_gb = estimated_bytes / (1024**3)
    free_gb = free / (1024**3)
    ctx_suffix = f" [{context}]" if context else ""
    return (
        f"insufficient disk space for YUV decode{ctx_suffix}: "
        f"estimated {est_gb:.1f} GB, "
        f"free {free_gb:.1f} GB on {workdir}. "
        f"Re-run with --workdir /path/to/volume-with-space "
        f"(or set VMAFTUNE_WORKDIR=/path/to/volume-with-space)"
    )


def _absolute_crf_range(adapter: object) -> tuple[int, int]:
    """Return the encoder's accepted CRF range for the bisect search.

    Prefer (in order):

    1. The encoder registry's ``absolute_range`` for the codec name
       (ADR-3388; curated per-codec encoder limits, see comment above).
    2. The adapter's own ``crf_min`` / ``crf_max`` attributes when both
       are present (libsvtav1 exposes these as the encoder absolute
       limits, distinct from the informative ``quality_range``).
    3. The adapter's ``quality_range`` as a last resort — keeps codecs
       without an absolute-range entry working at the informative window.

    The bisect calls this only when the caller did not pass
    ``crf_range`` explicitly. Callers that need the legacy informative-
    window behaviour pass ``crf_range=adapter.quality_range`` directly.
    """
    name = getattr(adapter, "name", "")
    table = encoder_registry.absolute_range(str(name))
    if table is not None:
        return table
    crf_min = getattr(adapter, "crf_min", None)
    crf_max = getattr(adapter, "crf_max", None)
    if crf_min is not None and crf_max is not None:
        return (int(crf_min), int(crf_max))
    qr = getattr(adapter, "quality_range", (0, 51))
    return (int(qr[0]), int(qr[1]))


@dataclasses.dataclass(frozen=True)
class BisectSample:
    """One per-iteration (CRF, bitrate, VMAF) probe collected by the bisect.

    The full bisect typically encodes 3-5 CRFs before converging on the
    target-meeting cell. Each probe is a genuine measurement on the
    codec under test (no extrapolation, no overshoot bias) — exactly
    the data the rate-quality chart should plot to avoid the
    connect-the-dots artefact described in ADR-0534. Failed encodes /
    score round-trips never reach this list; see :func:`_encode_and_score`.
    """

    crf: int
    bitrate_kbps: float
    vmaf_score: float
    encode_time_ms: float = 0.0


@dataclasses.dataclass(frozen=True)
class BisectResult:
    """One bisect's best (CRF, VMAF, bitrate) tuple at a given target.

    Mirrors the shape of :class:`vmaftune.compare.RecommendResult` so
    a one-line adapter (:func:`make_bisect_predicate`) satisfies the
    ``compare.PredicateFn`` signature.

    ``ok=False`` carries a human-readable ``error`` string and leaves
    the numeric fields at sentinel values; downstream consumers
    (``compare`` ranking, ``ladder`` knee selection) skip such rows.

    ``samples`` carries every successful encode+score probe the bisect
    walked through before converging on ``best_crf``. Consumers like
    the rate-quality chart use the raw samples instead of the
    (potentially overshoot-biased) picked-CRF point to draw a
    monotonic R-Q curve (ADR-0534). The tuple is empty when the bisect
    short-circuits before any sample completes (e.g. unknown codec).
    """

    codec: str
    best_crf: int
    measured_vmaf: float
    bitrate_kbps: float
    encode_time_ms: float
    n_iterations: int
    encoder_version: str = ""
    ok: bool = True
    error: str = ""
    samples: tuple[BisectSample, ...] = ()
    # ADR-0624 / ADR-0615 NR pre-scoring telemetry.
    # fr_calls_total: how many full-reference VMAF calls were made.
    # fr_calls_saved: how many were skipped due to NR early-elimination.
    # Both are 0 when --fast-nr was not active.
    fr_calls_total: int = 0
    fr_calls_saved: int = 0

    def to_recommend_result(self) -> RecommendResult:
        """Project onto the ``compare.RecommendResult`` shape.

        Lazy import keeps the bisect module standalone — ``compare``
        imports ``bisect`` for production wiring; the reverse import
        only happens when callers explicitly ask for the projection.
        """
        from .compare import RecommendResult

        return RecommendResult(
            codec=self.codec,
            best_crf=self.best_crf,
            bitrate_kbps=self.bitrate_kbps,
            encode_time_ms=self.encode_time_ms,
            vmaf_score=self.measured_vmaf,
            encoder_version=self.encoder_version,
            ok=self.ok,
            error=self.error,
            bisect_samples=tuple(
                {
                    "crf": int(s.crf),
                    "bitrate_kbps": float(s.bitrate_kbps),
                    "vmaf_score": float(s.vmaf_score),
                    "encode_time_ms": float(s.encode_time_ms),
                }
                for s in self.samples
            ),
        )


def _failure(
    codec: str,
    error: str,
    *,
    n_iterations: int = 0,
    best_crf: int = -1,
    measured_vmaf: float = float("nan"),
    bitrate_kbps: float = float("nan"),
    encode_time_ms: float = float("nan"),
    encoder_version: str = "",
    samples: tuple[BisectSample, ...] = (),
    fr_calls_total: int = 0,
    fr_calls_saved: int = 0,
) -> BisectResult:
    return BisectResult(
        codec=codec,
        best_crf=best_crf,
        measured_vmaf=measured_vmaf,
        bitrate_kbps=bitrate_kbps,
        encode_time_ms=encode_time_ms,
        n_iterations=n_iterations,
        encoder_version=encoder_version,
        ok=False,
        error=error,
        samples=samples,
        fr_calls_total=fr_calls_total,
        fr_calls_saved=fr_calls_saved,
    )


def _midpoint_lower_quality(lo: int, hi: int) -> int:
    """Round toward the lower-quality (higher-CRF) end of the window.

    Higher CRF = lower quality. ``ceil((lo + hi) / 2)`` always picks
    the higher-CRF mid when the window is even-sized — that way the
    "best so far" we accept on a pass is the CRF we actually measured,
    never one we extrapolated to from an adjacent sample.
    """
    return (lo + hi + 1) // 2


# Sentinel prefix embedded in BisectResult.error to signal that _encode_and_score
# performed NR early elimination (encode+decode-distorted done, FR skipped).
# The caller uses startswith() to detect this and extracts direction + calibrated
# NR-VMAF score from the remainder: "<_NR_SKIP_SENTINEL><direction>;<nr_vmaf>".
_NR_SKIP_SENTINEL: str = "__nr_skip__:"


def _try_nr_early_elimination_on_yuv(
    *,
    nr_proxy_backend: NRProxyBackend,
    distorted_yuv: Path,
    width: int,
    height: int,
    pix_fmt: str,
    target_vmaf: float,
) -> tuple[str, float] | None:
    """Run NR inference on an already-decoded distorted YUV and decide.

    Called after encode+decode-distorted but before FR scoring.  Returns
    ``(direction, nr_vmaf)`` when the calibrated NR-VMAF score is outside
    the δ_fast uncertainty zone (the FR call can be skipped). Returns
    ``None`` when the calibrated NR-VMAF score is inside the zone or
    inference fails (fall through to the full FR path).

    ``direction`` is ``"tighter"`` (raise CRF — quality above target)
    or ``"looser"`` (lower CRF — quality below target).
    """
    from .score_backend import NRProxyBackendError

    try:
        result = nr_proxy_backend.score_nr(
            distorted_yuv,
            width=width,
            height=height,
            pix_fmt=pix_fmt,
        )
        nr_score = result.nr_score
    except NRProxyBackendError as exc:
        _log.debug(
            "fast-nr: NR inference failed for %s: %s — falling through to FR",
            distorted_yuv,
            exc,
        )
        return None

    if nr_proxy_backend.is_far_from_target(nr_score, target_vmaf):
        direction = nr_proxy_backend.nr_implied_direction(nr_score, target_vmaf)
        return direction, nr_proxy_backend.calibrated_vmaf_score(nr_score)

    nr_vmaf = nr_proxy_backend.calibrated_vmaf_score(nr_score)
    _log.debug(
        "fast-nr: NR_raw=%.2f NR_VMAF=%.2f target=%.2f δ=%.1f — within "
        "uncertainty zone, paying FR cost",
        nr_score,
        nr_vmaf,
        target_vmaf,
        nr_proxy_backend.calibration_threshold,
    )
    return None


@dataclasses.dataclass
class _BisectLoop:
    """Fixed inputs and mutable state of one :func:`bisect_target_vmaf` run."""

    src: Path
    codec: str
    adapter: object
    preset: str
    target_vmaf: float
    max_iterations: int
    sem: threading.Semaphore
    encode_kwargs: dict[str, Any]
    nr_proxy_backend: NRProxyBackend | None
    yuv_est_bytes: int | None
    workdir: Path
    cur_lo: int
    cur_hi: int
    n_iterations: int = 0
    best: BisectResult | None = None
    last_vmaf_at_crf: dict[int, float] = dataclasses.field(default_factory=dict)
    # ADR-0534: every successful probe is kept so compare-sweep and the
    # rate-quality chart can plot the codec's real R-Q curve.
    samples: list[BisectSample] = dataclasses.field(default_factory=list)
    fr_calls_total: int = 0
    fr_calls_saved: int = 0


def _open_workdir(workdir: Path | None) -> tuple[tempfile.TemporaryDirectory[str] | None, Path]:
    """Return ``(temp-dir context or None, workdir path)``."""
    if workdir is not None:
        workdir_path = Path(workdir)
        workdir_path.mkdir(parents=True, exist_ok=True)
        return None, workdir_path
    # ADR-0598: prefer VMAFTUNE_WORKDIR (e.g. /probes/vmaftune-work in the
    # dev-mcp container) over the OS default /tmp, an 8 GB tmpfs there that a
    # full 1080p60 reference decode (~118 GB) cannot fit.
    parent = _workdir_parent()
    if parent is not None:
        parent.mkdir(parents=True, exist_ok=True)
    ctx = tempfile.TemporaryDirectory(dir=parent)
    return ctx, Path(ctx.name)


def _cleanup_workdir(
    workdir_ctx: tempfile.TemporaryDirectory[str] | None, workdir_path: Path, src: Path
) -> None:
    """ADR-0577 aggressive cleanup: drop the decoded reference YUV.

    It is re-decoded for the next codec's bisect; this caps peak disk use at
    one reference YUV instead of one per concurrent codec bisect. A temp-dir
    workdir is removed whole; a caller-supplied one loses only the
    ``<stem>.ref.decoded.yuv`` that ``_encode_and_score`` materialised.
    """
    if workdir_ctx is None:
        ref_yuv = workdir_path / (Path(src).stem + ".ref.decoded.yuv")
        with contextlib.suppress(OSError):
            if ref_yuv.exists():
                ref_yuv.unlink()
    else:
        workdir_ctx.cleanup()


def _midrun_disk_error(loop: _BisectLoop) -> str | None:
    """ADR-0577 / ADR-0641 mid-run disk-space check; an error text or ``None``.

    Container sources need 2x the estimated YUV size because the reference
    and distorted decodes can coexist; a raw source needs only the distorted
    decode plus overhead.
    """
    if loop.yuv_est_bytes is None:
        return None
    ctx = f"{loop.codec} @ VMAF {loop.target_vmaf:g}, iteration {loop.n_iterations}"
    return _check_disk_space(
        loop.workdir,
        estimated_bytes=loop.yuv_est_bytes,
        headroom=_midrun_disk_headroom(Path(loop.src)),
        context=ctx,
    )


def _probe_midpoint(loop: _BisectLoop, mid: int) -> BisectResult:
    """One encode+score at ``mid``, holding the decode semaphore (ADR-0577).

    The semaphore caps concurrent reference-YUV materialisation across the
    compare thread pool. NR pre-scoring (ADR-0624 / ADR-0615) is offered
    only while the window holds more than one candidate, so the final CRF
    always gets a full-reference confirmation.
    """
    use_nr = loop.nr_proxy_backend is not None and loop.cur_lo < loop.cur_hi
    with loop.sem:
        return _encode_and_score(
            src=loop.src,
            codec=loop.codec,
            adapter=loop.adapter,
            preset=loop.preset,
            crf=mid,
            workdir=loop.workdir,
            nr_proxy_backend=loop.nr_proxy_backend if use_nr else None,
            nr_target_vmaf=loop.target_vmaf if use_nr else None,
            **loop.encode_kwargs,
        )


def _apply_nr_skip(loop: _BisectLoop, sample: BisectResult, mid: int) -> None:
    """Advance the window in the NR-implied direction (FR call skipped)."""
    loop.fr_calls_saved += 1
    parts = sample.error[len(_NR_SKIP_SENTINEL) :].split(";", 1)
    direction = parts[0] if parts else "looser"
    try:
        nr_val = float(parts[1]) if len(parts) > 1 else float("nan")
    except ValueError:
        nr_val = float("nan")
    _log.info(
        "fast-nr: CRF %d NR_VMAF=%.2f target=%.2f δ=%.1f → %s (FR skipped, iter %d)",
        mid,
        nr_val,
        loop.target_vmaf,
        loop.nr_proxy_backend.calibration_threshold,  # type: ignore[union-attr]
        direction,
        loop.n_iterations,
    )
    if direction == "tighter":
        loop.cur_lo = mid + 1
    else:
        loop.cur_hi = mid - 1


def _record_sample(loop: _BisectLoop, sample: BisectResult, mid: int) -> BisectResult | None:
    """Store a probe, check monotonicity, narrow the window; failure or ``None``."""
    loop.samples.append(
        BisectSample(
            crf=int(mid),
            bitrate_kbps=float(sample.bitrate_kbps),
            vmaf_score=float(sample.measured_vmaf),
            encode_time_ms=float(sample.encode_time_ms),
        )
    )
    mono_err = _detect_monotonicity_violation(loop.last_vmaf_at_crf, mid, sample.measured_vmaf)
    loop.last_vmaf_at_crf[mid] = sample.measured_vmaf
    best = loop.best
    if mono_err is not None:
        return _failure(
            loop.codec,
            mono_err,
            n_iterations=loop.n_iterations,
            best_crf=best.best_crf if best is not None else -1,
            measured_vmaf=best.measured_vmaf if best is not None else float("nan"),
            bitrate_kbps=best.bitrate_kbps if best is not None else float("nan"),
            encode_time_ms=sample.encode_time_ms,
            encoder_version=sample.encoder_version,
            samples=tuple(loop.samples),
            fr_calls_total=loop.fr_calls_total,
            fr_calls_saved=loop.fr_calls_saved,
        )
    if sample.measured_vmaf >= loop.target_vmaf:
        # Quality met: best so far; try harder compression next.
        loop.best = dataclasses.replace(sample, n_iterations=loop.n_iterations)
        loop.cur_lo = mid + 1
    else:
        loop.cur_hi = mid - 1
    return None


def _bisect_iteration(loop: _BisectLoop) -> BisectResult | None:
    """Run one bisect step; a terminal result ends the search, ``None`` continues."""
    mid = _midpoint_lower_quality(loop.cur_lo, loop.cur_hi)
    loop.n_iterations += 1
    space_err = _midrun_disk_error(loop)
    if space_err is not None:
        return _failure(
            loop.codec,
            space_err,
            n_iterations=loop.n_iterations,
            samples=tuple(loop.samples),
        )
    sample = _probe_midpoint(loop, mid)
    # NR early elimination returns a sentinel failure carrying the direction.
    if not sample.ok and sample.error.startswith(_NR_SKIP_SENTINEL):
        _apply_nr_skip(loop, sample, mid)
        return None
    loop.fr_calls_total += 1
    if not sample.ok:
        return dataclasses.replace(
            sample,
            n_iterations=loop.n_iterations,
            samples=tuple(loop.samples),
            fr_calls_total=loop.fr_calls_total,
            fr_calls_saved=loop.fr_calls_saved,
        )
    return _record_sample(loop, sample, mid)


def _bisect_finish(loop: _BisectLoop, lo: int, hi: int) -> BisectResult:
    """Result after the loop: the best sample, or the unreachable-target failure."""
    if loop.nr_proxy_backend is not None:
        _log.info(
            "fast-nr: bisect done — FR calls %d total, %d saved (%.0f%%)",
            loop.fr_calls_total,
            loop.fr_calls_saved,
            100.0 * loop.fr_calls_saved / max(1, loop.fr_calls_total + loop.fr_calls_saved),
        )
    if loop.best is None:
        return _failure(
            loop.codec,
            (
                f"target VMAF {loop.target_vmaf:g} unreachable in CRF window "
                f"[{lo}, {hi}] after {loop.n_iterations} iterations "
                f"(best sample: {_describe_best_miss(loop.last_vmaf_at_crf)})"
            ),
            n_iterations=loop.n_iterations,
            samples=tuple(loop.samples),
            fr_calls_total=loop.fr_calls_total,
            fr_calls_saved=loop.fr_calls_saved,
        )
    return dataclasses.replace(
        loop.best,
        samples=tuple(loop.samples),
        fr_calls_total=loop.fr_calls_total,
        fr_calls_saved=loop.fr_calls_saved,
    )


def _bisect_search(
    src: Path,
    codec: str,
    adapter: object,
    target_vmaf: float,
    *,
    crf_range: tuple[int, int] | None,
    preset: str | None,
    max_iterations: int,
    knobs: dict[str, Any],
    workdir: Path | None,
    decode_semaphore: threading.Semaphore | None,
    nr_proxy_backend: NRProxyBackend | None,
) -> BisectResult:
    """Validate the window, open the workdir and run the bisect loop."""
    # ADR-0538: the caller's ``crf_range`` always wins, so --crf-min /
    # --crf-max and the tutorial fixtures keep their explicit windows.
    lo, hi = crf_range if crf_range is not None else _absolute_crf_range(adapter)
    lo, hi = int(lo), int(hi)
    if lo > hi:
        return _failure(codec, f"invalid crf_range: lo={lo} > hi={hi}")
    if max_iterations <= 0:
        return _failure(codec, f"max_iterations must be >= 1, got {max_iterations}")
    workdir_ctx, workdir_path = _open_workdir(workdir)
    # ADR-0577: the YUV size is estimated once for the mid-run disk checks.
    yuv_est = None
    if float(knobs["duration_s"]) > 0.0:
        yuv_est = _estimate_yuv_bytes(
            width=knobs["width"],
            height=knobs["height"],
            pix_fmt=knobs["pix_fmt"],
            fps=knobs["framerate"],
            duration_s=float(knobs["duration_s"]),
        )
    loop = _BisectLoop(
        src=src,
        codec=codec,
        adapter=adapter,
        preset=preset if preset is not None else _default_preset(adapter),
        target_vmaf=target_vmaf,
        max_iterations=max_iterations,
        sem=decode_semaphore if decode_semaphore is not None else _decode_semaphore,
        encode_kwargs=knobs,
        nr_proxy_backend=nr_proxy_backend,
        yuv_est_bytes=yuv_est,
        workdir=workdir_path,
        cur_lo=lo,
        cur_hi=hi,
    )
    try:
        while loop.cur_lo <= loop.cur_hi and loop.n_iterations < loop.max_iterations:
            early = _bisect_iteration(loop)
            if early is not None:
                return early
        return _bisect_finish(loop, lo, hi)
    finally:
        _cleanup_workdir(workdir_ctx, workdir_path, src)


def bisect_target_vmaf(
    src: Path,
    codec: str,
    target_vmaf: float,
    *,
    width: int,
    height: int,
    pix_fmt: str = "yuv420p",
    framerate: float = 24.0,
    duration_s: float = 0.0,
    sample_clip_seconds: float = 0.0,
    preset: str | None = None,
    crf_range: tuple[int, int] | None = None,
    max_iterations: int = 8,
    vmaf_model: str = DEFAULT_MODEL,
    score_backend: str | None = None,
    encode_runner: object | None = None,
    score_runner: object | None = None,
    decode_runner: object | None = None,
    ffmpeg_bin: str = "ffmpeg",
    vmaf_bin: str = "vmaf",
    workdir: Path | None = None,
    decode_semaphore: threading.Semaphore | None = None,
    nr_proxy_backend: NRProxyBackend | None = None,
) -> BisectResult:
    """Find the largest CRF whose measured VMAF still meets ``target_vmaf``.

    ``src`` is the reference YUV (geometry, ``pix_fmt``, ``framerate`` and
    ``duration_s`` come as kwargs); ``crf_range=None`` takes the encoder's
    absolute range (ADR-0538); ``sample_clip_seconds`` (ADR-0301) scores a
    centre window only. ``decode_semaphore`` gates concurrent reference
    decodes (ADR-0577; default: the module-level ``Semaphore(1)``).
    ``nr_proxy_backend`` enables NR pre-scoring (ADR-0624 / ADR-0615): a
    midpoint far from the target skips the full-reference call, and the
    result carries ``fr_calls_saved`` / ``fr_calls_total``. Runner kwargs are
    subprocess test seams; ``workdir=None`` uses a temp dir.

    Returns the best-so-far :class:`BisectResult`; ``ok=False`` when the
    target is unreachable in the window or monotonicity fails.
    """
    try:
        adapter = get_adapter(codec)
    except KeyError as exc:
        return _failure(codec, f"unknown codec: {exc}")
    # fmt: off
    knobs = {
        "width": width, "height": height, "pix_fmt": pix_fmt, "framerate": framerate,
        "duration_s": duration_s, "sample_clip_seconds": sample_clip_seconds,
        "vmaf_model": vmaf_model, "score_backend": score_backend,
        "encode_runner": encode_runner, "score_runner": score_runner,
        "decode_runner": decode_runner, "ffmpeg_bin": ffmpeg_bin, "vmaf_bin": vmaf_bin,
    }
    # fmt: on
    # fmt: off
    return _bisect_search(
        src, codec, adapter, target_vmaf, crf_range=crf_range, preset=preset,
        max_iterations=max_iterations, knobs=knobs, workdir=workdir,
        decode_semaphore=decode_semaphore, nr_proxy_backend=nr_proxy_backend,
    )
    # fmt: on


def _default_preset(adapter: object) -> str:
    """Return the adapter's mid-range preset.

    The codec-adapter contract names ``"medium"`` for the canonical
    cross-codec sweep axis (see AGENTS.md "Adapter preset vocabulary"),
    so we prefer that when the adapter advertises it; otherwise we
    pick the middle of the ``presets`` tuple.
    """
    presets = getattr(adapter, "presets", None)
    if not presets:
        return "medium"
    if "medium" in presets:
        return "medium"
    return presets[len(presets) // 2]


def _detect_monotonicity_violation(
    history: dict[int, float],
    new_crf: int,
    new_vmaf: float,
) -> str | None:
    """Detect a 2-sample violation of monotone-decreasing VMAF in CRF.

    Returns ``None`` when consistent; a human-readable error string
    when at least one prior sample directly contradicts the new one
    by more than a small float-noise tolerance.
    """
    tol = 0.5  # VMAF units — looser than measurement noise on a single shot
    for crf, vmaf in history.items():
        if crf < new_crf and new_vmaf > vmaf + tol:
            return (
                f"monotonicity violation: VMAF rose from {vmaf:.2f} at CRF {crf} "
                f"to {new_vmaf:.2f} at CRF {new_crf} (expected non-increasing)"
            )
        if crf > new_crf and new_vmaf < vmaf - tol:
            return (
                f"monotonicity violation: VMAF fell from {vmaf:.2f} at CRF {crf} "
                f"to {new_vmaf:.2f} at CRF {new_crf} (expected non-decreasing for lower CRF)"
            )
    return None


def _describe_best_miss(history: dict[int, float]) -> str:
    if not history:
        return "no samples recorded"
    crf, vmaf = max(history.items(), key=lambda kv: kv[1])
    return f"closest miss VMAF={vmaf:.2f} at CRF {crf}"


def _sample_clip_window(
    *,
    duration_s: float,
    sample_clip_seconds: float,
    framerate: float,
) -> tuple[float, float, int, int]:
    """Return encode/score alignment knobs for ADR-0301 sample clips."""
    sample_s = float(sample_clip_seconds)
    duration = float(duration_s)
    fps = float(framerate)
    if sample_s <= 0.0 or duration <= 0.0 or sample_s >= duration or fps <= 0.0:
        return 0.0, 0.0, 0, 0
    clip_s = sample_s
    start_s = max(0.0, (duration - clip_s) / 2.0)
    frame_skip_ref = max(0, round(start_s * fps))
    frame_cnt = max(1, round(clip_s * fps))
    return start_s, clip_s, frame_skip_ref, frame_cnt


def _check_adapter_cell(adapter: object, codec: str, preset: str, crf: int) -> BisectResult | None:
    """Reject a preset or CRF the encoder cannot take; ``None`` when fine.

    ADR-0538: ``adapter.validate(preset, crf)`` would also enforce the
    adapter's informative ``quality_range`` (x265 ``(15, 40)``, svtav1
    ``(20, 50)``). The bisect window is the wider absolute range so the
    premium-archival targets are reachable, hence the preset whitelist and
    the encoder's own limits are checked here instead.
    """
    abs_lo, abs_hi = _absolute_crf_range(adapter)
    if not abs_lo <= int(crf) <= abs_hi:
        return _failure(
            codec,
            (
                f"adapter rejected (preset={preset!r}, crf={crf}): "
                f"crf outside encoder absolute range [{abs_lo}, {abs_hi}]"
            ),
        )
    presets = getattr(adapter, "presets", ())
    if presets and preset not in presets:
        return _failure(
            codec,
            (
                f"adapter rejected (preset={preset!r}, crf={crf}): "
                f"unknown preset; expected one of {presets}"
            ),
        )
    return None


def _bisect_encode_request(
    src: Path,
    cell: tuple[object, str, str, int, Path],
    geometry: tuple[int, int, str, float],
    clip: tuple[float, float, int, int],
) -> EncodeRequest:
    """The :class:`EncodeRequest` for one bisect cell.

    ``cell`` is ``(adapter, codec, preset, crf, output)``, ``geometry`` is
    ``(width, height, pix_fmt, framerate)``, ``clip`` is
    :func:`_sample_clip_window`'s tuple. Bug #1: a container reference must
    not get ``-f rawvideo`` (ffmpeg would parse the demuxed container as raw
    YUV and write an empty file), so the container suffix is detected here.
    """
    adapter, codec, preset, crf, out_path = cell
    width, height, pix_fmt, framerate = geometry
    return EncodeRequest(
        source=Path(src),
        width=int(width),
        height=int(height),
        pix_fmt=pix_fmt,
        framerate=float(framerate),
        encoder=getattr(adapter, "encoder", codec),
        preset=preset,
        crf=int(crf),
        output=out_path,
        sample_clip_seconds=clip[1],
        sample_clip_start_s=clip[0],
        source_is_container=Path(src).suffix.lower() not in VMAF_RAW_SUFFIXES,
    )


def _encode_failure_message(enc_res: Any, enc_req: EncodeRequest, crf: int) -> str:
    """Tell a missing encoder binary apart from a genuine encode failure.

    ADR-0498: ffmpeg exits non-zero for both; the stderr tail decides.
    """
    encoder_name = enc_req.encoder
    stderr_tail = enc_res.stderr_tail or ""
    last_line = stderr_tail.strip().splitlines()[-1] if stderr_tail else "no stderr"
    lowered = stderr_tail.lower()
    if "encoder not found" in lowered or "unknown encoder" in lowered or "no such codec" in lowered:
        return f"encoder unavailable ({encoder_name}): {last_line}"
    return f"encode failed at CRF {crf} (exit={enc_res.exit_status}): {last_line}"


def _enc_failure(codec: str, enc_res: Any, error: str) -> BisectResult:
    """A failed sample that still reports the encode's time and version."""
    return _failure(
        codec,
        error,
        encode_time_ms=enc_res.encode_time_ms,
        encoder_version=enc_res.encoder_version,
    )


def _prepare_reference(
    src: Path,
    workdir: Path,
    src_is_container: bool,
    geometry: tuple[int, int, str, float, float],
    ffmpeg_bin: str,
    runner: object | None,
) -> tuple[Path | None, str | None]:
    """The reference libvmaf can read: ``src`` itself, or its raw-YUV decode.

    Bug #3: libvmaf takes raw .yuv / .y4m only, so a container reference is
    decoded once into the workdir and reused by every iteration of the
    bisect. ``geometry`` is ``(width, height, pix_fmt, framerate,
    duration_s)``. Returns ``(path, None)`` or ``(None, error)``.
    """
    if not src_is_container:
        return Path(src), None
    from .score import _decode_to_raw_yuv

    width, height, pix_fmt, framerate, duration_s = geometry
    decoded_ref = workdir / (Path(src).stem + ".ref.decoded.yuv")
    rc = 0
    if not decoded_ref.exists():
        # ADR-0598: preflight disk check; a 1080p60 634 s source decodes to
        # ~118 GB, and the dev-mcp /tmp is an 8 GB tmpfs. Skipped when the
        # duration is unknown (the ffmpeg return code reports ENOSPC).
        decode_dur = float(duration_s) if float(duration_s) > 0.0 else None
        if decode_dur is not None:
            workdir.mkdir(parents=True, exist_ok=True)
            est = _estimate_yuv_bytes(
                width=width,
                height=height,
                pix_fmt=pix_fmt,
                fps=framerate,
                duration_s=decode_dur,
            )
            space_err = _check_disk_space(workdir, estimated_bytes=est)
            if space_err is not None:
                return None, space_err
        # BBB e2e v2 Bug #v2-A: clamp the decode to ``duration_s`` (a 10 s
        # probe of a 634 s source is ~896 MB, not ~58 GB); 0 keeps the legacy
        # full-source decode.
        rc = _decode_to_raw_yuv(
            Path(src),
            decoded_ref,
            pix_fmt=pix_fmt,
            ffmpeg_bin=ffmpeg_bin,
            runner=runner,
            duration_s=decode_dur,
        )
    if rc != 0 or not decoded_ref.exists():
        return None, f"reference decode to raw YUV failed (rc={rc}) for {src}"
    return decoded_ref, None


def _drop_artifacts(out_path: Path, distorted: Path) -> None:
    """Best-effort removal of the encode and its per-iteration decoded sidecar."""
    with contextlib.suppress(OSError):
        if out_path.exists():
            out_path.unlink()
        if distorted != out_path and distorted.exists():
            distorted.unlink()


def _nr_skip_sample(
    codec: str, enc_res: Any, out_path: Path, score_req: ScoreRequest, score_args: dict[str, Any]
) -> BisectResult | None:
    """ADR-0624 / ADR-0615 NR early elimination before the full-reference call.

    Returns the sentinel failure the bisect loop reads as "advance the
    window without a real failure", or ``None`` to score in full.
    """
    nr_backend, nr_target = score_args["nr_proxy_backend"], score_args["nr_target_vmaf"]
    if nr_backend is None or nr_target is None:
        return None
    nr_result = _try_nr_early_elimination_on_yuv(
        nr_proxy_backend=nr_backend,
        distorted_yuv=score_req.distorted,
        width=int(score_args["width"]),
        height=int(score_args["height"]),
        pix_fmt=score_args["pix_fmt"],
        target_vmaf=nr_target,
    )
    if nr_result is None:
        return None
    _drop_artifacts(out_path, score_req.distorted)
    return _enc_failure(codec, enc_res, f"{_NR_SKIP_SENTINEL}{nr_result[0]};{nr_result[1]:.6f}")


def _score_encoded(
    *,
    codec: str,
    crf: int,
    out_path: Path,
    enc_res: Any,
    ref_for_score: Path,
    sample_duration_s: float,
    window: tuple[int, int],
    score_args: dict[str, Any],
) -> BisectResult:
    """Decode the encode, optionally NR-skip, score it, and build the sample."""
    width, height = int(score_args["width"]), int(score_args["height"])
    pix_fmt, duration_s = score_args["pix_fmt"], score_args["duration_s"]
    workdir, ffmpeg_bin = score_args["workdir"], score_args["ffmpeg_bin"]
    score_req = ScoreRequest(
        reference=ref_for_score,
        distorted=out_path,
        width=width,
        height=height,
        pix_fmt=pix_fmt,
        model=score_args["vmaf_model"],
        frame_skip_ref=window[0],
        frame_cnt=window[1],
        # BBB e2e v2 Bug #v2-A: cap the distorted decode at the window length.
        duration_s=float(duration_s),
    )
    # libvmaf takes raw .yuv / .y4m only; a no-op for raw encoder output.
    score_req, decode_rc = maybe_decode_distorted(
        score_req, workdir=workdir, ffmpeg_bin=ffmpeg_bin, runner=score_args["decode_runner"]
    )
    if decode_rc != 0:
        _drop_artifacts(out_path, out_path)
        return _enc_failure(
            codec, enc_res, f"distorted decode to raw YUV failed (rc={decode_rc}) at CRF {crf}"
        )
    skipped = _nr_skip_sample(codec, enc_res, out_path, score_req, score_args)
    if skipped is not None:
        return skipped
    score_res = run_score(
        score_req,
        vmaf_bin=score_args["vmaf_bin"],
        runner=score_args["score_runner"],
        backend=score_args["score_backend"],
    )
    _drop_artifacts(out_path, score_req.distorted)
    return _sample_from_score(codec, crf, enc_res, score_res, sample_duration_s, duration_s)


def _sample_from_score(
    codec: str,
    crf: int,
    enc_res: Any,
    score_res: Any,
    sample_duration_s: float,
    duration_s: float,
) -> BisectResult:
    """Validate the score and fold it with the encode into a sample result."""
    if score_res.exit_status != 0:
        return _enc_failure(
            codec, enc_res, f"score failed at CRF {crf} (exit={score_res.exit_status})"
        )
    measured = float(score_res.vmaf_score)
    if math.isnan(measured) or measured < _VMAF_VALID_FLOOR or measured > _VMAF_VALID_CEIL:
        return _enc_failure(
            codec, enc_res, f"score returned out-of-range VMAF {measured!r} at CRF {crf}"
        )
    bitrate_duration_s = sample_duration_s if sample_duration_s > 0.0 else duration_s
    return BisectResult(
        codec=codec,
        best_crf=int(crf),
        measured_vmaf=measured,
        bitrate_kbps=bitrate_kbps(enc_res.encode_size_bytes, bitrate_duration_s),
        encode_time_ms=enc_res.encode_time_ms,
        n_iterations=0,
        encoder_version=enc_res.encoder_version,
        ok=True,
        error="",
    )


def _encode_and_score(
    *,
    src: Path,
    codec: str,
    adapter: object,
    preset: str,
    crf: int,
    width: int,
    height: int,
    pix_fmt: str,
    framerate: float,
    duration_s: float,
    sample_clip_seconds: float,
    vmaf_model: str,
    score_backend: str | None,
    encode_runner: object | None = None,
    score_runner: object | None = None,
    ffmpeg_bin: str,
    vmaf_bin: str,
    workdir: Path,
    decode_runner: object | None = None,
    nr_proxy_backend: NRProxyBackend | None = None,
    nr_target_vmaf: float | None = None,
) -> BisectResult:
    """One encode+score round-trip — returns a sample-shaped BisectResult.

    ``n_iterations`` is always ``0`` (the caller stamps it). The decode
    runner defaults to the encode runner (both are ffmpeg). An NR skip
    returns ``ok=False`` with ``error = _NR_SKIP_SENTINEL + "<dir>;<nr>"``.
    """
    if (bad := _check_adapter_cell(adapter, codec, preset, crf)) is not None:
        return bad
    out_path = workdir / f"bisect_{codec}_{preset}_{crf}.mkv"
    clip = _sample_clip_window(
        duration_s=duration_s, sample_clip_seconds=sample_clip_seconds, framerate=framerate
    )
    enc_req = _bisect_encode_request(
        src, (adapter, codec, preset, crf, out_path), (width, height, pix_fmt, framerate), clip
    )
    enc_res = run_encode(enc_req, ffmpeg_bin=ffmpeg_bin, runner=encode_runner)
    if enc_res.exit_status != 0:
        return _enc_failure(codec, enc_res, _encode_failure_message(enc_res, enc_req, crf))
    decode = decode_runner if decode_runner is not None else encode_runner
    ref, ref_err = _prepare_reference(
        src, workdir, enc_req.source_is_container, (width, height, pix_fmt, framerate, duration_s),
        ffmpeg_bin, decode,
    )  # fmt: skip
    if ref is None:
        return _enc_failure(codec, enc_res, str(ref_err))
    score_args = {
        "width": width, "height": height, "pix_fmt": pix_fmt, "duration_s": duration_s,
        "workdir": workdir, "ffmpeg_bin": ffmpeg_bin, "vmaf_model": vmaf_model,
        "decode_runner": decode, "nr_proxy_backend": nr_proxy_backend,
        "nr_target_vmaf": nr_target_vmaf, "vmaf_bin": vmaf_bin,
        "score_runner": score_runner, "score_backend": score_backend,
    }  # fmt: skip
    return _score_encoded(
        codec=codec, crf=crf, out_path=out_path, enc_res=enc_res, ref_for_score=ref,
        sample_duration_s=clip[1], window=(clip[2], clip[3]), score_args=score_args,
    )  # fmt: skip


def make_bisect_predicate(
    target_vmaf: float,
    *,
    width: int,
    height: int,
    pix_fmt: str = "yuv420p",
    framerate: float = 24.0,
    duration_s: float = 0.0,
    sample_clip_seconds: float = 0.0,
    preset: str | None = None,
    crf_range: tuple[int, int] | None = None,
    max_iterations: int = 8,
    vmaf_model: str = DEFAULT_MODEL,
    score_backend: str | None = None,
    encode_runner: object | None = None,
    score_runner: object | None = None,
    decode_runner: object | None = None,
    ffmpeg_bin: str = "ffmpeg",
    vmaf_bin: str = "vmaf",
    workdir: Path | None = None,
    decode_semaphore: threading.Semaphore | None = None,
    nr_proxy_backend: NRProxyBackend | None = None,
) -> PredicateFn:
    """Return a :data:`compare.PredicateFn` that closes over bisect knobs.

    The callable matches ``compare.compare_codecs``'s predicate signature
    ``(codec, src, target_vmaf) -> RecommendResult``. The target the
    predicate receives wins; the closure-time ``target_vmaf`` is the default
    when it receives NaN. ``decode_semaphore`` is shared across the compare
    thread pool (ADR-0577), ``nr_proxy_backend`` enables NR pre-scoring
    (ADR-0624 / ADR-0615); see :func:`bisect_target_vmaf`.
    """
    # fmt: off
    knobs: dict[str, Any] = {
        "width": width, "height": height, "pix_fmt": pix_fmt, "framerate": framerate,
        "duration_s": duration_s, "sample_clip_seconds": sample_clip_seconds,
        "preset": preset, "crf_range": crf_range, "max_iterations": max_iterations,
        "vmaf_model": vmaf_model, "score_backend": score_backend,
        "encode_runner": encode_runner, "score_runner": score_runner,
        "decode_runner": decode_runner, "ffmpeg_bin": ffmpeg_bin, "vmaf_bin": vmaf_bin,
        "workdir": workdir, "decode_semaphore": decode_semaphore,
        "nr_proxy_backend": nr_proxy_backend,
    }
    # fmt: on

    def _predicate(codec: str, src: Path, runtime_target_vmaf: float) -> RecommendResult:
        # The runtime target wins; NaN means "use the closure default".
        target = runtime_target_vmaf if not math.isnan(runtime_target_vmaf) else target_vmaf
        return bisect_target_vmaf(src, codec, float(target), **knobs).to_recommend_result()

    return _predicate


__all__ = [
    "DEFAULT_MAX_CONCURRENT_DECODES",
    "BisectResult",
    "BisectSample",
    "bisect_target_vmaf",
    "make_bisect_predicate",
    "set_decode_semaphore",
]
