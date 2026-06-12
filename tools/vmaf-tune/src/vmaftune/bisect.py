# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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
from typing import TYPE_CHECKING

from .codec_adapters import get_adapter
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
    global _decode_semaphore  # noqa: PLW0603 — module-level singleton
    if max_concurrent < 1:
        raise ValueError(f"max_concurrent must be >= 1, got {max_concurrent}")
    _decode_semaphore = threading.Semaphore(max_concurrent)


# Sentinel: a measured VMAF below this floor against a non-degenerate
# encode signals a sample failure, not a real low-quality result. We
# refuse to draw a monotonicity conclusion from such samples.
_VMAF_VALID_FLOOR: float = 0.0
_VMAF_VALID_CEIL: float = 100.0


# ADR-0538 — Encoder-absolute CRF ranges per codec, used as the bisect
# search window when the caller passes ``crf_range=None``. These are the
# bounds the encoder will accept at the FFmpeg CLI, NOT the
# perceptually-informative window adapters expose via
# :attr:`CodecAdapter.quality_range`. The premium-archival defaults
# (``--target-vmafs 94,96,97,98``) frequently require CRFs below the
# informative window — e.g. libsvtav1's ``quality_range = (20, 50)`` is
# too tight to ever reach VMAF 97. The bisect therefore searches the
# absolute range so high targets are reachable, and falls back to the
# adapter's ``quality_range`` when no override exists for the codec.
#
# Sources (see docs/research/2026-05-18-premium-vmaf-bisect.md):
#   libx264, libx265 : ``-crf 0..51`` (man x264; FFmpeg encoder doc)
#   libvpx-vp9       : ``-crf 0..63`` (FFmpeg encoder doc)
#   libaom-av1       : ``-crf 0..63`` (FFmpeg encoder doc)
#   libsvtav1        : ``-crf 0..63`` (matches adapter.crf_min/crf_max)
#
# Hardware encoders (NVENC / AMF / QSV / VideoToolbox) and VVenC are
# omitted from this table; their adapters either expose narrower native
# quality ranges (CQ / QP scales that don't map to 0..63) or refuse
# CRF 0 by design. For those codecs we fall back to the adapter's
# ``quality_range`` until per-codec validation rules land.
_ABSOLUTE_CRF_RANGE_BY_NAME: dict[str, tuple[int, int]] = {
    "libx264": (0, 51),
    "libx265": (0, 51),
    "libvpx-vp9": (0, 63),
    "libaom-av1": (0, 63),
    "libsvtav1": (0, 63),
}


def _workdir_parent() -> Path | None:
    """Return the preferred parent directory for temporary work directories.

    Resolution order (ADR-0549):

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
        "VMAFTUNE_WORKDIR=%s is not writable (uid=%d); "
        "falling back to OS default temp directory",
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

    Used for the preflight disk-space check (ADR-0549). The estimate
    is intentionally rounded up — we multiply by the ceiling of fps and
    add a small per-frame overhead for alignment, so the check is
    conservative rather than optimistic.
    """
    bpp = _BYTES_PER_PIXEL.get(pix_fmt, _BYTES_PER_PIXEL_DEFAULT)
    frames = max(1, int(math.ceil(fps * max(duration_s, 0.0))))
    return int(math.ceil(width * height * bpp * frames))


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
    required = int(math.ceil(estimated_bytes * headroom))
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

    1. The codec-name lookup in :data:`_ABSOLUTE_CRF_RANGE_BY_NAME`
       (curated per-codec encoder limits, see module docstring above).
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
    table = _ABSOLUTE_CRF_RANGE_BY_NAME.get(str(name))
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
    connect-the-dots artefact described in ADR-0530. Failed encodes /
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
    monotonic R-Q curve (ADR-0530). The tuple is empty when the bisect
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

    def to_recommend_result(self) -> "RecommendResult":
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
    nr_proxy_backend: "NRProxyBackend",
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
    vmaf_model: str = "vmaf_v0.6.1",
    score_backend: str | None = None,
    encode_runner: object | None = None,
    score_runner: object | None = None,
    decode_runner: object | None = None,
    ffmpeg_bin: str = "ffmpeg",
    vmaf_bin: str = "vmaf",
    workdir: Path | None = None,
    decode_semaphore: threading.Semaphore | None = None,
    nr_proxy_backend: "NRProxyBackend | None" = None,
) -> BisectResult:
    """Find the largest CRF whose measured VMAF still meets ``target_vmaf``.

    Parameters
    ----------
    src
        Reference YUV. Geometry / pix_fmt / framerate / duration are
        passed via kwargs because the file does not self-describe.
    codec
        Codec adapter name (must exist in
        :mod:`vmaftune.codec_adapters`).
    target_vmaf
        Quality floor; the bisect returns the highest-CRF cell whose
        measured VMAF clears this.
    sample_clip_seconds
        Optional ADR-0301 centre-window sample clip. When positive and
        shorter than ``duration_s``, each iteration encodes only that
        window and scores against the matching reference frame window.
    crf_range
        ``(lo, hi)`` inclusive bound on the search domain. ``None``
        defaults to the encoder's **absolute** CRF range per
        :func:`_absolute_crf_range` (ADR-0538, supersedes the
        ADR-0296 ``quality_range`` default). The wider absolute range
        is required so the high-VMAF targets in the premium-archival
        sweep (``--target-vmafs 94,96,97,98``) are reachable —
        adapters such as ``libsvtav1`` declare
        ``quality_range = (20, 50)`` for the informative window, which
        is too tight to bisect down to VMAF >= 95. Callers that need
        the historical informative-window behaviour pass
        ``crf_range=adapter.quality_range`` explicitly.
    max_iterations
        Hard cap on encode+score round-trips. The window halves each
        iteration so the asymptote is ``ceil(log2(hi - lo + 1))``;
        ``max_iterations`` short-circuits before that for paranoia.
    preset
        Preset name forwarded verbatim to the adapter. ``None`` picks
        the adapter's mid-range default (``"medium"`` for x264 /
        x265 / svtav1 today).
    encode_runner / score_runner
        Subprocess-runner stubs. Default to
        :func:`subprocess.run` via the underlying ``run_encode`` /
        ``run_score`` calls. Tests inject fakes; production callers
        leave them ``None``.
    workdir
        Where the per-iteration encoded outputs live. ``None`` uses a
        :class:`tempfile.TemporaryDirectory` cleaned at exit.
    decode_semaphore
        A :class:`threading.Semaphore` that gates concurrent
        reference-YUV decode operations (ADR-0577). When ``None``,
        the module-level ``_decode_semaphore`` is used (default:
        serial, i.e. ``Semaphore(1)``). Callers that want multiple
        concurrent decodes pass ``threading.Semaphore(N)`` or call
        :func:`set_decode_semaphore` before spawning the thread pool.
    nr_proxy_backend
        Optional :class:`~vmaftune.score_backend.NRProxyBackend` for
        fast NR pre-scoring (ADR-0624 / ADR-0615). When provided, each
        bisect midpoint is first scored via the cheap NR proxy. If
        ``|NR - target| > δ_fast``, the full-reference VMAF call is
        skipped and the bisect window advances in the NR-implied
        direction. Full-reference scoring always runs for the *final*
        confirmed CRF and for any midpoint within the δ_fast uncertainty
        zone. The result carries ``fr_calls_saved`` / ``fr_calls_total``
        telemetry fields. Pass ``None`` (default) to disable NR
        pre-scoring and use full-reference scoring throughout.

    Returns
    -------
    BisectResult
        The best-so-far (CRF, VMAF, bitrate) tuple. ``ok=False`` when
        the target is unreachable in the given window or the
        monotonicity assumption fails.  When ``nr_proxy_backend`` is
        supplied, ``fr_calls_total`` and ``fr_calls_saved`` carry
        the NR telemetry.
    """
    try:
        adapter = get_adapter(codec)
    except KeyError as exc:
        return _failure(codec, f"unknown codec: {exc}")

    # ADR-0577: use caller-supplied semaphore or fall back to the
    # module-level singleton. The module-level default is Semaphore(1)
    # (serial decodes) unless the CLI called set_decode_semaphore().
    effective_sem: threading.Semaphore = (
        decode_semaphore if decode_semaphore is not None else _decode_semaphore
    )

    # ADR-0538: default to the encoder's absolute CRF range (e.g. 0..51
    # for libx264 / libx265, 0..63 for libvpx-vp9 / libaom-av1 /
    # libsvtav1) rather than the adapter's perceptually-informative
    # ``quality_range``. Premium-archival targets (VMAF 94..98) require
    # CRFs below the informative window for most codecs; the absolute
    # range makes them reachable. Caller-supplied ``crf_range`` always
    # wins so the existing --crf-min / --crf-max CLI knobs and the
    # codec-tutorial test fixtures keep their explicit windows.
    lo, hi = crf_range if crf_range is not None else _absolute_crf_range(adapter)
    lo = int(lo)
    hi = int(hi)
    if lo > hi:
        return _failure(codec, f"invalid crf_range: lo={lo} > hi={hi}")

    if max_iterations <= 0:
        return _failure(codec, f"max_iterations must be >= 1, got {max_iterations}")

    chosen_preset = preset if preset is not None else _default_preset(adapter)

    if workdir is None:
        # ADR-0549: prefer VMAFTUNE_WORKDIR (e.g. /probes/vmaftune-work
        # in the dev-mcp container, ~435 GB free) over the OS default
        # /tmp (8 GB tmpfs in the container — too small for a full
        # 1080p60 BBB YUV decode of ~118 GB).
        _wdir_parent = _workdir_parent()
        if _wdir_parent is not None:
            _wdir_parent.mkdir(parents=True, exist_ok=True)
        workdir_ctx = tempfile.TemporaryDirectory(dir=_wdir_parent)
        workdir_path = Path(workdir_ctx.name)
    else:
        workdir_ctx = None
        workdir_path = Path(workdir)
        workdir_path.mkdir(parents=True, exist_ok=True)

    # State across iterations:
    best: BisectResult | None = None
    last_vmaf_at_crf: dict[int, float] = {}
    # ADR-0530: record every successful encode+score round-trip so
    # downstream consumers (compare-sweep, rate-quality chart) can
    # plot the genuine codec R-Q curve instead of just the picked-CRF
    # cell. Duplicates by (crf) are kept — the bisect never revisits
    # a CRF in normal operation but a deliberate retry would be a real
    # second measurement worth preserving.
    samples: list[BisectSample] = []
    n_iterations = 0
    cur_lo, cur_hi = lo, hi
    # ADR-0624 NR telemetry counters.
    _fr_calls_total: int = 0
    _fr_calls_saved: int = 0

    # ADR-0577: estimate YUV size once for mid-run disk checks.
    _yuv_est_bytes: int | None = None
    if float(duration_s) > 0.0:
        _yuv_est_bytes = _estimate_yuv_bytes(
            width=width,
            height=height,
            pix_fmt=pix_fmt,
            fps=framerate,
            duration_s=float(duration_s),
        )

    try:
        while cur_lo <= cur_hi and n_iterations < max_iterations:
            mid = _midpoint_lower_quality(cur_lo, cur_hi)
            n_iterations += 1

            # ADR-0577 / ADR-0641: mid-run disk-space check. Container
            # sources need 2× the estimated YUV size because the reference
            # and distorted decodes can coexist. A pre-decoded/raw source
            # already occupies the reference side, so each iteration needs
            # only the distorted decode plus normal file overhead.
            # ``workdir_path`` is bound on every branch of the
            # if/else above, so a None-check would be dead code.
            if _yuv_est_bytes is not None:
                _ctx = f"{codec} @ VMAF {target_vmaf:g}, iteration {n_iterations}"
                _space_err = _check_disk_space(
                    workdir_path,
                    estimated_bytes=_yuv_est_bytes,
                    headroom=_midrun_disk_headroom(Path(src)),
                    context=_ctx,
                )
                if _space_err is not None:
                    return _failure(
                        codec,
                        _space_err,
                        n_iterations=n_iterations,
                        samples=tuple(samples),
                    )

            # ADR-0624 / ADR-0615 — NR pre-scoring fast path.
            # When the caller supplied an NRProxyBackend, _encode_and_score
            # is told about it. Inside _encode_and_score, after the
            # encode+decode-distorted step but before the FR libvmaf call,
            # the NR backend scores the distorted YUV. If |NR - target| >
            # δ_fast the function returns early with nr_skipped=True and no
            # measured_vmaf; the loop advances the window in the NR-implied
            # direction without paying the FR cost.
            #
            # The ``cur_lo < cur_hi`` guard ensures the final CRF (window
            # has collapsed to one candidate) always gets a FR confirmation.
            _use_nr = nr_proxy_backend is not None and cur_lo < cur_hi

            # ADR-0577: acquire the decode semaphore before calling
            # _encode_and_score. The semaphore gates the number of
            # concurrent reference-YUV materialisation operations across
            # all threads in the compare thread pool. Encoder runs inside
            # _encode_and_score proceed without semaphore gating — only
            # the decode step benefits from the cap.
            with effective_sem:
                sample = _encode_and_score(
                    src=src,
                    codec=codec,
                    adapter=adapter,
                    preset=chosen_preset,
                    crf=mid,
                    width=width,
                    height=height,
                    pix_fmt=pix_fmt,
                    framerate=framerate,
                    duration_s=duration_s,
                    sample_clip_seconds=sample_clip_seconds,
                    vmaf_model=vmaf_model,
                    score_backend=score_backend,
                    encode_runner=encode_runner,
                    score_runner=score_runner,
                    decode_runner=decode_runner,
                    ffmpeg_bin=ffmpeg_bin,
                    vmaf_bin=vmaf_bin,
                    workdir=workdir_path,
                    nr_proxy_backend=nr_proxy_backend if _use_nr else None,
                    nr_target_vmaf=target_vmaf if _use_nr else None,
                )

            # NR early-elimination path: _encode_and_score returned a
            # sentinel BisectResult with ok=False and error starting with
            # _NR_SKIP_SENTINEL. Parse direction + calibrated NR-VMAF from the payload.
            if not sample.ok and sample.error.startswith(_NR_SKIP_SENTINEL):
                _fr_calls_saved += 1
                _payload = sample.error[len(_NR_SKIP_SENTINEL) :]
                _parts = _payload.split(";", 1)
                direction = _parts[0] if _parts else "looser"
                try:
                    nr_val = float(_parts[1]) if len(_parts) > 1 else float("nan")
                except ValueError:
                    nr_val = float("nan")
                _log.info(
                    "fast-nr: CRF %d NR_VMAF=%.2f target=%.2f δ=%.1f → %s " "(FR skipped, iter %d)",
                    mid,
                    nr_val,
                    target_vmaf,
                    nr_proxy_backend.calibration_threshold,  # type: ignore[union-attr]
                    direction,
                    n_iterations,
                )
                if direction == "tighter":
                    cur_lo = mid + 1
                else:
                    cur_hi = mid - 1
                continue

            _fr_calls_total += 1

            if not sample.ok:
                return dataclasses.replace(
                    sample,
                    n_iterations=n_iterations,
                    samples=tuple(samples),
                    fr_calls_total=_fr_calls_total,
                    fr_calls_saved=_fr_calls_saved,
                )

            # ADR-0530: record every successful probe (regardless of
            # whether it cleared the target) so the rate-quality chart
            # can render the actual codec curve.
            samples.append(
                BisectSample(
                    crf=int(mid),
                    bitrate_kbps=float(sample.bitrate_kbps),
                    vmaf_score=float(sample.measured_vmaf),
                    encode_time_ms=float(sample.encode_time_ms),
                )
            )

            mono_err = _detect_monotonicity_violation(last_vmaf_at_crf, mid, sample.measured_vmaf)
            last_vmaf_at_crf[mid] = sample.measured_vmaf
            if mono_err is not None:
                return _failure(
                    codec,
                    mono_err,
                    n_iterations=n_iterations,
                    best_crf=best.best_crf if best is not None else -1,
                    measured_vmaf=best.measured_vmaf if best is not None else float("nan"),
                    bitrate_kbps=best.bitrate_kbps if best is not None else float("nan"),
                    encode_time_ms=sample.encode_time_ms,
                    encoder_version=sample.encoder_version,
                    samples=tuple(samples),
                    fr_calls_total=_fr_calls_total,
                    fr_calls_saved=_fr_calls_saved,
                )

            if sample.measured_vmaf >= target_vmaf:
                # We met quality at this CRF — record it as best-so-far
                # and try harder compression next.
                best = dataclasses.replace(sample, n_iterations=n_iterations)
                cur_lo = mid + 1
            else:
                # Quality miss — narrow toward higher quality.
                cur_hi = mid - 1

        if nr_proxy_backend is not None:
            _log.info(
                "fast-nr: bisect done — FR calls %d total, %d saved (%.0f%%)",
                _fr_calls_total,
                _fr_calls_saved,
                100.0 * _fr_calls_saved / max(1, _fr_calls_total + _fr_calls_saved),
            )

        if best is None:
            # Target unreachable in the searched window.
            return _failure(
                codec,
                (
                    f"target VMAF {target_vmaf:g} unreachable in CRF window "
                    f"[{lo}, {hi}] after {n_iterations} iterations "
                    f"(best sample: {_describe_best_miss(last_vmaf_at_crf)})"
                ),
                n_iterations=n_iterations,
                samples=tuple(samples),
                fr_calls_total=_fr_calls_total,
                fr_calls_saved=_fr_calls_saved,
            )

        return dataclasses.replace(
            best,
            samples=tuple(samples),
            fr_calls_total=_fr_calls_total,
            fr_calls_saved=_fr_calls_saved,
        )
    finally:
        # ADR-0577 aggressive cleanup: after the bisect completes (all
        # iterations for this codec at this target), delete the decoded
        # reference YUV. It is re-decoded on the next codec's bisect.
        # This costs one extra decode per codec but caps peak disk usage
        # to one reference YUV at a time instead of N (where N = number
        # of concurrent codec bisects running in parallel). At 110 GB per
        # 1080p BBB source, serial cleanup drops peak from 330 GB (3
        # codecs × 110 GB) to 110 GB.
        if workdir_ctx is None:
            # Caller-supplied workdir: clean up the decoded ref YUV that
            # _encode_and_score materialized (stem + ".ref.decoded.yuv").
            # The temp-dir case is handled by workdir_ctx.cleanup() below,
            # which removes the entire tree.
            _ref_yuv = workdir_path / (Path(src).stem + ".ref.decoded.yuv")
            with contextlib.suppress(OSError):
                if _ref_yuv.exists():
                    _ref_yuv.unlink()
        if workdir_ctx is not None:
            workdir_ctx.cleanup()


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
    frame_skip_ref = max(0, int(round(start_s * fps)))
    frame_cnt = max(1, int(round(clip_s * fps)))
    return start_s, clip_s, frame_skip_ref, frame_cnt


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
    nr_proxy_backend: "NRProxyBackend | None" = None,
    nr_target_vmaf: float | None = None,
) -> BisectResult:
    """One encode+score round-trip — returns a sample-shaped BisectResult.

    The ``n_iterations`` field on the returned struct is always ``0``;
    the caller stamps it with the cumulative count.

    When ``nr_proxy_backend`` is supplied, NR early-elimination is
    attempted after encode+decode-distorted but before the FR libvmaf
    call. If the NR score is outside the δ_fast uncertainty zone the
    function returns early with ``ok=False`` and
    ``error=_NR_SKIP_SENTINEL + "<direction>;<nr_score>"`` — the caller
    detects this sentinel, increments ``_fr_calls_saved``, and advances
    the bisect window in the NR-implied direction without treating the
    result as a real failure.
    """
    # ADR-0538: ``adapter.validate(preset, crf)`` enforces both the
    # preset whitelist AND the adapter's perceptually-informative
    # ``quality_range`` (e.g. x265's ``(15, 40)``, svtav1's
    # ``(20, 50)``). For the bisect we want the preset check but NOT
    # the informative-range gate — the search window in
    # :func:`bisect_target_vmaf` is the encoder's absolute CRF range,
    # which is intentionally wider than the informative window so
    # premium-archival targets are reachable. Validate the preset by
    # itself first; then re-run the full validator under a "swallow
    # CRF-range complaints" rule so genuine encoder limits (e.g.
    # libsvtav1's ``crf_min/crf_max``) still fire when the bisect
    # was misconfigured with an out-of-encoder window.
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

    out_path = workdir / f"bisect_{codec}_{preset}_{crf}.mkv"
    encoder_name = getattr(adapter, "encoder", codec)
    sample_start_s, sample_duration_s, frame_skip_ref, frame_cnt = _sample_clip_window(
        duration_s=duration_s,
        sample_clip_seconds=sample_clip_seconds,
        framerate=framerate,
    )
    # Bug #1: When the reference source is a container (mp4/mkv/…) the
    # encoder ffmpeg invocation must NOT prepend ``-f rawvideo`` —
    # otherwise ffmpeg tries to parse the demuxed container as raw YUV
    # and produces "Output file is empty". Autodetect via the same
    # suffix table that the post-encode decode step uses.
    src_is_container = Path(src).suffix.lower() not in VMAF_RAW_SUFFIXES
    enc_req = EncodeRequest(
        source=Path(src),
        width=int(width),
        height=int(height),
        pix_fmt=pix_fmt,
        framerate=float(framerate),
        encoder=encoder_name,
        preset=preset,
        crf=int(crf),
        output=out_path,
        sample_clip_seconds=sample_duration_s,
        sample_clip_start_s=sample_start_s,
        source_is_container=src_is_container,
    )
    enc_res = run_encode(enc_req, ffmpeg_bin=ffmpeg_bin, runner=encode_runner)
    if enc_res.exit_status != 0:
        # ADR-0498 / BBB e2e v2 follow-up #6: ffmpeg returns the same
        # non-zero exit for "encoder binary missing in this build" as
        # for genuine encode failures (rate-control overflow, etc.).
        # Distinguish them via the stderr tail so operators see
        # "encoder unavailable" rather than "Encoder not found" /
        # "encode failed" for the libsvtav1 case in the dev-mcp image.
        stderr_tail = enc_res.stderr_tail or ""
        last_line = stderr_tail.strip().splitlines()[-1] if stderr_tail else "no stderr"
        lowered = stderr_tail.lower()
        if (
            "encoder not found" in lowered
            or "unknown encoder" in lowered
            or "no such codec" in lowered
        ):
            err_msg = f"encoder unavailable ({encoder_name}): {last_line}"
        else:
            err_msg = f"encode failed at CRF {crf} (exit={enc_res.exit_status}): {last_line}"
        return _failure(
            codec,
            err_msg,
            encode_time_ms=enc_res.encode_time_ms,
            encoder_version=enc_res.encoder_version,
        )

    # Bug #3: The libvmaf CLI only accepts raw .yuv / .y4m. The
    # encoded artefact is a Matroska container; without this decode
    # step the vmaf binary mis-parses it as raw YUV and aborts with
    # "file too small for declared geometry". We also need a raw YUV
    # reference for the same reason — if ``src`` is a container, the
    # caller cannot have decoded it (bisect is responsible for the
    # full round trip), so decode it once into the workdir.
    # ``decode_runner`` defaults to the encode runner: both are
    # ffmpeg invocations, so production callers (which leave both
    # ``None``) get the real ``subprocess.run`` either way, while
    # tests can keep injecting a single stub.
    effective_decode_runner = decode_runner if decode_runner is not None else encode_runner

    ref_for_score = Path(src)
    decoded_ref: Path | None = None
    if src_is_container:
        from .score import _decode_to_raw_yuv  # noqa: PLC0415 — module-local helper

        decoded_ref = workdir / (Path(src).stem + ".ref.decoded.yuv")
        # Re-use across iterations within the same bisect — workdir
        # persists for the bisect's lifetime so a single decode is
        # enough (every iteration scores the same reference).
        rc = 0
        if not decoded_ref.exists():
            # ADR-0549: preflight disk-space check before materialising
            # the raw YUV decode. A 1080p60 634 s BBB source decodes to
            # ~118 GB; the dev-mcp container's /tmp is an 8 GB tmpfs, so
            # the decode fails with rc=228 (ENOSPC) without this guard.
            # The check is skipped when duration_s <= 0 (unknown
            # duration) — we cannot estimate in that case and the error
            # will surface via the ffmpeg returncode as before.
            _decode_dur_s = float(duration_s) if float(duration_s) > 0.0 else None
            if _decode_dur_s is not None:
                workdir.mkdir(parents=True, exist_ok=True)
                _est = _estimate_yuv_bytes(
                    width=width,
                    height=height,
                    pix_fmt=pix_fmt,
                    fps=framerate,
                    duration_s=_decode_dur_s,
                )
                _space_err = _check_disk_space(workdir, estimated_bytes=_est)
                if _space_err is not None:
                    return _failure(
                        codec,
                        _space_err,
                        encode_time_ms=enc_res.encode_time_ms,
                        encoder_version=enc_res.encoder_version,
                    )
            # BBB e2e v2 Bug #v2-A: clamp the reference decode to
            # ``duration_s`` so a 10 s probe against a 634 s source
            # produces ~896 MB of raw YUV, not ~58 GB. ``duration_s == 0``
            # preserves the legacy full-source behaviour for callers
            # that have not bound a source duration yet.
            decode_dur = float(duration_s) if float(duration_s) > 0.0 else None
            rc = _decode_to_raw_yuv(
                Path(src),
                decoded_ref,
                pix_fmt=pix_fmt,
                ffmpeg_bin=ffmpeg_bin,
                runner=effective_decode_runner,
                duration_s=decode_dur,
            )
        if rc != 0 or not decoded_ref.exists():
            return _failure(
                codec,
                f"reference decode to raw YUV failed (rc={rc}) for {src}",
                encode_time_ms=enc_res.encode_time_ms,
                encoder_version=enc_res.encoder_version,
            )
        ref_for_score = decoded_ref

    score_req = ScoreRequest(
        reference=ref_for_score,
        distorted=out_path,
        width=int(width),
        height=int(height),
        pix_fmt=pix_fmt,
        model=vmaf_model,
        frame_skip_ref=frame_skip_ref,
        frame_cnt=frame_cnt,
        # BBB e2e v2 Bug #v2-A: thread the requested duration so the
        # ``maybe_decode_distorted`` step caps the raw-YUV decode at
        # the analysed window length.
        duration_s=float(duration_s),
    )
    # Decode the encoded container to raw YUV — libvmaf will not accept
    # the .mkv otherwise. ``maybe_decode_distorted`` is a no-op for raw
    # outputs, so callers that wire a custom encoder that emits .yuv
    # directly are unaffected.
    score_req, decode_rc = maybe_decode_distorted(
        score_req,
        workdir=workdir,
        ffmpeg_bin=ffmpeg_bin,
        runner=effective_decode_runner,
    )
    if decode_rc != 0:
        with contextlib.suppress(OSError):
            if out_path.exists():
                out_path.unlink()
        return _failure(
            codec,
            f"distorted decode to raw YUV failed (rc={decode_rc}) at CRF {crf}",
            encode_time_ms=enc_res.encode_time_ms,
            encoder_version=enc_res.encoder_version,
        )

    # ADR-0624 / ADR-0615 — NR early-elimination: the distorted YUV is
    # now available (score_req.distorted). Run NR inference; if the score
    # is far from the target skip the FR libvmaf call and return a
    # sentinel so the bisect loop can advance the window cheaply.
    if nr_proxy_backend is not None and nr_target_vmaf is not None:
        _nr_result = _try_nr_early_elimination_on_yuv(
            nr_proxy_backend=nr_proxy_backend,
            distorted_yuv=score_req.distorted,
            width=width,
            height=height,
            pix_fmt=pix_fmt,
            target_vmaf=nr_target_vmaf,
        )
        if _nr_result is not None:
            _direction, _nr_score = _nr_result
            # Clean up artefacts before returning the sentinel.
            with contextlib.suppress(OSError):
                if out_path.exists():
                    out_path.unlink()
                if score_req.distorted != out_path and score_req.distorted.exists():
                    score_req.distorted.unlink()
            return _failure(
                codec,
                f"{_NR_SKIP_SENTINEL}{_direction};{_nr_score:.6f}",
                encode_time_ms=enc_res.encode_time_ms,
                encoder_version=enc_res.encoder_version,
            )

    score_res = run_score(
        score_req,
        vmaf_bin=vmaf_bin,
        runner=score_runner,
        backend=score_backend,
    )

    # Best-effort cleanup: the encoded artefact + per-iteration decoded
    # sidecar are throwaway; we keep the workdir alive across
    # iterations so a caller-supplied workdir can still inspect it
    # later (the temp-dir path cleans on context exit instead).
    with contextlib.suppress(OSError):
        if out_path.exists():
            out_path.unlink()
        if score_req.distorted != out_path and score_req.distorted.exists():
            score_req.distorted.unlink()

    if score_res.exit_status != 0:
        return _failure(
            codec,
            f"score failed at CRF {crf} (exit={score_res.exit_status})",
            encode_time_ms=enc_res.encode_time_ms,
            encoder_version=enc_res.encoder_version,
        )

    measured = float(score_res.vmaf_score)
    if math.isnan(measured) or measured < _VMAF_VALID_FLOOR or measured > _VMAF_VALID_CEIL:
        return _failure(
            codec,
            f"score returned out-of-range VMAF {measured!r} at CRF {crf}",
            encode_time_ms=enc_res.encode_time_ms,
            encoder_version=enc_res.encoder_version,
        )

    bitrate_duration_s = sample_duration_s if sample_duration_s > 0.0 else duration_s
    br_kbps = bitrate_kbps(enc_res.encode_size_bytes, bitrate_duration_s)

    return BisectResult(
        codec=codec,
        best_crf=int(crf),
        measured_vmaf=measured,
        bitrate_kbps=br_kbps,
        encode_time_ms=enc_res.encode_time_ms,
        n_iterations=0,
        encoder_version=enc_res.encoder_version,
        ok=True,
        error="",
    )


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
    vmaf_model: str = "vmaf_v0.6.1",
    score_backend: str | None = None,
    encode_runner: object | None = None,
    score_runner: object | None = None,
    decode_runner: object | None = None,
    ffmpeg_bin: str = "ffmpeg",
    vmaf_bin: str = "vmaf",
    workdir: Path | None = None,
    decode_semaphore: threading.Semaphore | None = None,
    nr_proxy_backend: "NRProxyBackend | None" = None,
) -> "PredicateFn":
    """Return a :data:`compare.PredicateFn` that closes over bisect knobs.

    The returned callable matches ``compare.compare_codecs``'s
    predicate signature ``(codec, src, target_vmaf) -> RecommendResult``.
    The ``target_vmaf`` argument the predicate receives at call time
    is forwarded through verbatim; the closure-time ``target_vmaf``
    here serves as the default for callers that pin one floor across
    many comparisons.

    Note ``target_vmaf`` appears at both layers because the predicate
    signature exposes a target argument (so the same predicate may be
    re-used with shifting targets) but encode geometry / runners must
    be fixed before the predicate is built.

    ``decode_semaphore`` is forwarded to :func:`bisect_target_vmaf`
    so callers that build predicates for multiple codecs share the same
    semaphore across all threads in the compare thread pool (ADR-0577).
    When ``None``, the module-level ``_decode_semaphore`` is used.

    ``nr_proxy_backend`` is an optional :class:`~vmaftune.score_backend.NRProxyBackend`
    for fast NR pre-scoring (ADR-0624 / ADR-0615). When supplied, each
    bisect midpoint is first scored via the NR proxy; midpoints far from
    the target skip the full-reference VMAF call. Pass ``None`` (default)
    to use full-reference scoring throughout.
    """

    def _predicate(codec: str, src: Path, runtime_target_vmaf: float) -> "RecommendResult":
        # Runtime target argument wins; closure-time default is unused
        # whenever ``compare_codecs`` calls us (it always supplies the
        # current target). We keep the closure default for callers that
        # bind the predicate directly without ``compare_codecs``.
        # ``runtime_target_vmaf`` is typed ``float`` so ``is None`` is
        # statically impossible; NaN is the only way the predicate could
        # ever request the closure default. Keeping the NaN branch is
        # still meaningful — callers can signal "use my bound default"
        # by passing ``float('nan')``.
        target = runtime_target_vmaf if not math.isnan(runtime_target_vmaf) else target_vmaf
        result = bisect_target_vmaf(
            src,
            codec,
            float(target),
            width=width,
            height=height,
            pix_fmt=pix_fmt,
            framerate=framerate,
            duration_s=duration_s,
            sample_clip_seconds=sample_clip_seconds,
            preset=preset,
            crf_range=crf_range,
            max_iterations=max_iterations,
            vmaf_model=vmaf_model,
            score_backend=score_backend,
            encode_runner=encode_runner,
            score_runner=score_runner,
            decode_runner=decode_runner,
            ffmpeg_bin=ffmpeg_bin,
            vmaf_bin=vmaf_bin,
            workdir=workdir,
            decode_semaphore=decode_semaphore,
            nr_proxy_backend=nr_proxy_backend,
        )
        return result.to_recommend_result()

    return _predicate


__all__ = [
    "BisectResult",
    "BisectSample",
    "DEFAULT_MAX_CONCURRENT_DECODES",
    "bisect_target_vmaf",
    "make_bisect_predicate",
    "set_decode_semaphore",
]
