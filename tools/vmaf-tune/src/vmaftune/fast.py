# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Phase A.5 fast-path — proxy + Bayesian + GPU-verify recommend.

This module wires the production ``vmaf-tune fast`` subcommand
documented in :doc:`/adr/0276-vmaf-tune-fast-path` and
:doc:`/adr/0304-vmaf-tune-fast-path-prod-wiring` (production wiring).
The flow is:

1. **Optuna TPE search** over the integer CRF axis. The objective is
   ``|predicted_vmaf - target| + λ·predicted_kbps`` so ties break
   toward lower bitrate. Default budget is 30 trials (production) or
   :data:`SMOKE_N_TRIALS` (smoke).
2. **Proxy scoring** via :func:`vmaftune.proxy.run_proxy` — the
   production fr_regressor_v2 ONNX session (no smoke models in
   production mode). Each TPE trial encodes a short sample chunk,
   extracts the canonical-6 features, and predicts VMAF in
   microseconds.
3. **Single GPU verify pass at the end** — one real ffmpeg encode +
   libvmaf score at the recommended CRF using the GPU score backend
   from :mod:`vmaftune.score_backend`. This is mandatory; the proxy
   alone never wins. The verify score is authoritative; the proxy
   score is a diagnostic.

Smoke mode keeps the synthetic CRF→VMAF curve from the ADR-0276
scaffold so CI on hosts without onnxruntime / Optuna / a GPU still
exercises the search-loop wiring end-to-end. The slow Phase A grid
path stays canonical and untouched (ADR-0237 contract).
"""

from __future__ import annotations

import dataclasses
import math
import subprocess
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Any

# Optuna is an optional dependency, gated behind the ``[fast]`` install
# extra. Importing it lazily lets the rest of vmaftune import cleanly on
# hosts that never run the fast path.
try:  # pragma: no cover - import-guarded
    import optuna  # type: ignore[import-not-found]

    _OPTUNA_AVAILABLE = True
except ImportError:  # pragma: no cover - import-guarded
    optuna = None  # type: ignore[assignment]
    _OPTUNA_AVAILABLE = False


# Default CRF search range for x264. Other codecs override via the
# adapter once the production loop wires the codec-adapter registry.
DEFAULT_CRF_LO: int = 10
DEFAULT_CRF_HI: int = 51

# Sample-chunk duration used for proxy-grade encodes in the production
# loop. Documented here so the follow-up PR can lift it from a single
# constant rather than scattering magic numbers.
SAMPLE_CHUNK_SECONDS: float = 5.0

# Smoke mode synthesises this many trials so the Optuna wiring is
# exercised end-to-end. Match the speedup-model entry in Research-0060.
SMOKE_N_TRIALS: int = 50

# Production default — TPE converges in 30–50 trials on a single
# integer CRF axis (Research-0076 §1).
PROD_N_TRIALS: int = 30

# Default proxy/verify gap tolerance. When the GPU verify pass disagrees
# with the proxy by more than this many VMAF points, the recommendation
# is flagged OOD and the operator is expected to fall back to the slow
# Phase A grid (ADR-0276 fallback contract; Research-0076 §2).
DEFAULT_PROXY_TOLERANCE: float = 1.5


# ---------------------------------------------------------------------------
# Pluggable surfaces — production wiring + smoke-mode entry points.
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class TrialSample:
    """One ``(crf, predicted_vmaf, predicted_kbps)`` proposal.

    Production: filled by encoding a 5-second chunk, extracting
    canonical-6 features, and running ``fr_regressor_v2`` over the
    feature vector + codec one-hot. Smoke mode: synthesised by a
    deterministic mock.
    """

    crf: int
    predicted_vmaf: float
    predicted_kbps: float


@dataclasses.dataclass(frozen=True)
class FastRecommendResult:
    """Outcome of one ``fast_recommend`` call.

    ``verify_vmaf`` and ``proxy_verify_gap`` are populated when the
    production loop runs the GPU verify pass; smoke mode leaves them
    as ``None`` since no real encode/score happens.
    """

    encoder: str
    target_vmaf: float
    recommended_crf: int
    predicted_vmaf: float
    predicted_kbps: float
    n_trials: int
    smoke: bool
    notes: str = ""
    verify_vmaf: float | None = None
    proxy_verify_gap: float | None = None

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


def _smoke_predictor(crf: int) -> TrialSample:
    """Deterministic mock that mimics x264's monotone CRF→VMAF curve.

    Higher CRF → lower VMAF, lower bitrate. Shape is loosely calibrated
    against published x264 medium-preset behaviour on 1080p material:
    VMAF ≈ 100 at CRF 10, VMAF ≈ 50 at CRF 51, with a smooth taper.
    Used by the smoke-mode pipeline so the optimiser has a sensible
    objective without needing real weights.
    """
    crf_norm = (crf - DEFAULT_CRF_LO) / max(DEFAULT_CRF_HI - DEFAULT_CRF_LO, 1)
    # VMAF curve: smooth taper from ~99 at CRF 10 to ~52 at CRF 51.
    vmaf = 99.0 - 47.0 * (crf_norm**1.2)
    # Bitrate curve: exponential decay (typical for x264).
    kbps = 8000.0 * math.exp(-3.5 * crf_norm) + 80.0
    return TrialSample(crf=crf, predicted_vmaf=vmaf, predicted_kbps=kbps)


def _objective_factory(
    target_vmaf: float,
    predict: Callable[[int], TrialSample],
    crf_range: tuple[int, int],
) -> Callable[[Any], float]:
    """Build an Optuna objective that minimises ``|vmaf - target| + λ·kbps``.

    The bitrate term is weighted small relative to the quality term so
    the optimiser primarily hits the target; ties (multiple CRFs at the
    target) break toward the lower-bitrate option. This mirrors the
    typical "pick the lowest CRF that hits VMAF≥X" framing while
    staying differentiable enough for TPE.
    """
    crf_lo, crf_hi = crf_range
    bitrate_weight = 1.0e-4

    def _objective(trial: Any) -> float:
        crf = trial.suggest_int("crf", crf_lo, crf_hi)
        sample = predict(crf)
        trial.set_user_attr("predicted_vmaf", sample.predicted_vmaf)
        trial.set_user_attr("predicted_kbps", sample.predicted_kbps)
        quality_gap = abs(sample.predicted_vmaf - target_vmaf)
        return float(quality_gap + bitrate_weight * sample.predicted_kbps)

    return _objective


def _require_optuna() -> None:
    if not _OPTUNA_AVAILABLE:
        raise RuntimeError(
            "vmaf-tune fast requires Optuna. Install with: "
            "pip install 'vmaf-tune[fast]'  (see docs/usage/vmaf-tune.md)."
        )


def _proxy_score(
    features: list[float],
    *,
    encoder: str,
    preset_norm: float,
    crf_norm: float,
) -> float:
    """Run the production fr_regressor_v2 proxy on a feature vector.

    Thin wrapper over :func:`vmaftune.proxy.run_proxy` so callers — and
    tests — go through a single seam. The import is kept local to avoid
    pulling onnxruntime into module-level imports (the smoke path must
    keep working on hosts that never installed onnxruntime).
    """
    from vmaftune.proxy import run_proxy

    return run_proxy(
        features,
        encoder=encoder,
        preset_norm=preset_norm,
        crf_norm=crf_norm,
    )


def _build_prod_predictor(
    src: Path,
    encoder: str,
    crf_range: tuple[int, int],
    sample_extractor: Callable[[Path, int, str], tuple[list[float], float]] | None,
    backend: str | None = None,
) -> Callable[[int], TrialSample]:
    """Construct a CRF→TrialSample predictor backed by the v2 proxy.

    ``sample_extractor`` is the seam Phase B/C share for "encode a short
    chunk + extract canonical-6 + observe bitrate". Tests inject a fake;
    production callers leave it default and the harness builds it from
    the existing :mod:`vmaftune.encode` + libvmaf feature pipeline.

    ``backend`` is forwarded to :func:`_build_production_sample_extractor`
    so each TPE trial scores its probe clip on the same GPU backend used
    by the verify pass. ADR-0498 follow-up #7: wires the
    :mod:`vmaftune.score_backend` GPU path through to the proxy-encode
    extractor so all 30 TPE trial scores run on GPU when available.
    """
    if sample_extractor is None:
        sample_extractor = _build_production_sample_extractor(backend=backend)

    crf_lo, crf_hi = crf_range
    crf_span = max(crf_hi - crf_lo, 1)

    def _predict(crf: int) -> TrialSample:
        features, observed_kbps = sample_extractor(src, crf, encoder)
        crf_norm = (crf - crf_lo) / crf_span
        # Preset normalisation collapses to 0.5 (neutral) until the
        # caller threads --preset through; this mirrors the v2 training
        # contract default (Research-0076 §2).
        preset_norm = 0.5
        predicted_vmaf = _proxy_score(
            features,
            encoder=encoder,
            preset_norm=preset_norm,
            crf_norm=crf_norm,
        )
        return TrialSample(
            crf=crf,
            predicted_vmaf=float(predicted_vmaf),
            predicted_kbps=float(observed_kbps),
        )

    return _predict


def _gpu_verify(
    src: Path,
    encoder: str,
    crf: int,
    *,
    score_backend_select: Callable[..., str] | None = None,
    encode_runner: Callable[[Path, str, int, str], tuple[float, float]] | None = None,
) -> float:
    """Run ONE real encode + libvmaf score at the recommended CRF.

    The verify pass is mandatory — the proxy alone never wins
    (ADR-0304 invariant). On hosts with a GPU backend installed, the
    libvmaf score axis is collapsed by the configured backend
    (CUDA / SYCL / HIP); on GPU-less hosts the strict-mode selector
    falls back to CPU when ``prefer="auto"`` is passed.

    Parameters
    ----------
    src
        Source video path.
    encoder
        Codec name (e.g. ``libx264``).
    crf
        Recommended CRF from the TPE search.
    score_backend_select
        Test seam — defaults to :func:`vmaftune.score_backend.select_backend`.
    encode_runner
        Test seam — defaults to a thin wrapper over the existing
        :mod:`vmaftune.encode` + :mod:`vmaftune.score` pipeline. Returns
        ``(observed_kbps, vmaf_score)`` for the encode at ``crf``.

    Returns
    -------
    float
        Real libvmaf score for the chosen CRF.
    """
    if score_backend_select is None:
        from vmaftune.score_backend import select_backend

        score_backend_select = select_backend
    if encode_runner is None:
        encode_runner = _build_production_encode_runner()

    backend = score_backend_select(prefer="auto")
    _kbps, vmaf = encode_runner(src, encoder, crf, backend)
    return float(vmaf)


def _run_tpe(
    *,
    target_vmaf: float,
    predictor: Callable[[int], TrialSample],
    crf_range: tuple[int, int],
    n_trials: int,
    time_budget_s: float | None = None,
) -> tuple[int, float, float, int]:
    """Run the Optuna TPE search; return (recommended_crf, vmaf, kbps, trials)."""
    if time_budget_s is not None and time_budget_s <= 0.0:
        raise ValueError(f"time_budget_s must be > 0 when set; got {time_budget_s!r}")
    # Cross-procedural narrowing: callers gate every fast-path entry on
    # ``_require_optuna()`` which raises when the optional dep is
    # missing, so by the time we land here ``optuna`` is bound. The
    # assert is the cheapest way to surface that invariant to pyright
    # (and to fail loud if a future caller skips ``_require_optuna``).
    assert optuna is not None, "optuna not installed; call _require_optuna() first"
    objective = _objective_factory(target_vmaf, predictor, crf_range)

    # Suppress Optuna's default INFO-level chatter; the CLI is the
    # right place to surface progress.
    optuna.logging.set_verbosity(optuna.logging.WARNING)
    study = optuna.create_study(
        direction="minimize",
        sampler=optuna.samplers.TPESampler(seed=0),
    )
    study.optimize(
        objective,
        n_trials=n_trials,
        timeout=float(time_budget_s) if time_budget_s is not None else None,
        show_progress_bar=False,
    )

    best = study.best_trial
    recommended_crf = int(best.params["crf"])
    predicted_vmaf = float(best.user_attrs.get("predicted_vmaf", float("nan")))
    predicted_kbps = float(best.user_attrs.get("predicted_kbps", float("nan")))
    return recommended_crf, predicted_vmaf, predicted_kbps, len(study.trials)


# ---------------------------------------------------------------------------
# Production seam builders — wired through vmaftune.encode + vmaftune.score.
# Injected into fast_recommend when the caller does not supply an override.
# ---------------------------------------------------------------------------


def _build_production_sample_extractor(
    *,
    ffmpeg_bin: str = "ffmpeg",
    vmaf_bin: str = "vmaf",
    pix_fmt: str = "yuv420p",
    preset: str = "medium",
    backend: str | None = None,
) -> Callable[[Path, int, str], tuple[list[float], float]]:
    """Return a ``(src, crf, encoder) → (canonical_6, kbps)`` callable.

    Each call:
    1. Probes source geometry via ffprobe.
    2. Encodes a :data:`SAMPLE_CHUNK_SECONDS`-second centre window at
       ``crf`` using ``encoder`` / ``preset``.
    3. Scores the encoded clip against the same source window with the
       libvmaf CLI to extract the canonical-6 feature means.
    4. Returns ``(canonical_6_features, observed_kbps)``.

    ``backend`` selects the libvmaf scoring backend (``cpu`` / ``cuda``
    / ``sycl`` / ``hip`` / ``auto``).  When ``None`` or ``"auto"`` the
    libvmaf CLI picks the fastest available backend.  ADR-0498 follow-up
    #7: previously the sample extractor ignored the backend selected by
    :func:`vmaftune.score_backend.select_backend` so all 30 TPE trials
    scored on CPU even when a GPU was available; the ``backend`` kwarg
    wires the GPU path through to each probe-encode score call.

    The returned callable is stateless: parallel TPE trials can call it
    concurrently (each gets its own tempdir).
    """
    from . import CANONICAL6_FEATURES
    from .encode import EncodeRequest, bitrate_kbps, run_encode
    from .predictor_features import _probe_video_geometry
    from .score import ScoreRequest, run_score

    class _Cfg:
        ffprobe_bin: str = "ffprobe"

    cfg = _Cfg()
    _score_backend: str | None = None if (backend is None or backend == "auto") else backend

    def _extract(src: Path, crf: int, encoder: str) -> tuple[list[float], float]:
        with tempfile.TemporaryDirectory(prefix="vmaftune-fast-sample-") as td:
            tmpdir = Path(td)
            dist = tmpdir / "dist.mp4"

            width, height, fps = _probe_video_geometry(src, cfg, subprocess.run)  # type: ignore[arg-type]
            if width == 0 or height == 0 or fps == 0.0:
                raise RuntimeError(f"fast sample_extractor: ffprobe failed for {src}")

            # Locate the centre window; clip to source length if shorter.
            duration_s = SAMPLE_CHUNK_SECONDS
            is_container = src.suffix.lower() not in {".yuv", ".y4m", ""}
            enc_req = EncodeRequest(
                source=src,
                width=width,
                height=height,
                pix_fmt=pix_fmt,
                framerate=fps,
                encoder=encoder,
                preset=preset,
                crf=crf,
                output=dist,
                sample_clip_seconds=duration_s,
                source_is_container=is_container,
            )
            enc_result = run_encode(enc_req, ffmpeg_bin=ffmpeg_bin)
            if enc_result.exit_status != 0 or not dist.exists():
                raise RuntimeError(
                    f"fast sample_extractor: encode failed (CRF {crf}, "
                    f"encoder {encoder}): {enc_result.stderr_tail[-300:]}"
                )

            observed_kbps = bitrate_kbps(enc_result.encode_size_bytes, duration_s)

            score_req = ScoreRequest(
                reference=src,
                distorted=dist,
                width=width,
                height=height,
                pix_fmt=pix_fmt,
                # Mirror the same centre window the encoder used.
                frame_skip_ref=int(
                    enc_req.sample_clip_start_s * fps if enc_req.sample_clip_start_s > 0 else 0
                ),
                frame_cnt=int(duration_s * fps),
            )
            score_result = run_score(score_req, vmaf_bin=vmaf_bin, backend=_score_backend)
            if score_result.exit_status != 0:
                raise RuntimeError(
                    f"fast sample_extractor: score failed: {score_result.stderr_tail[-300:]}"
                )

            features = [
                score_result.feature_means.get(f, float("nan")) for f in CANONICAL6_FEATURES
            ]
            return features, observed_kbps

    return _extract


def _build_production_encode_runner(
    *,
    ffmpeg_bin: str = "ffmpeg",
    vmaf_bin: str = "vmaf",
    pix_fmt: str = "yuv420p",
    preset: str = "medium",
) -> Callable[[Path, str, int, str], tuple[float, float]]:
    """Return a ``(src, encoder, crf, backend) → (kbps, vmaf_score)`` callable.

    Used for the mandatory GPU verify pass at the end of fast_recommend.
    Encodes the full source, scores it, and returns the real kbps +
    libvmaf score so the caller can compute the proxy/verify gap.
    """
    from .encode import EncodeRequest, bitrate_kbps, run_encode
    from .predictor_features import _probe_video_geometry
    from .score import ScoreRequest, run_score

    class _Cfg:
        ffprobe_bin: str = "ffprobe"

    cfg = _Cfg()

    def _run(src: Path, encoder: str, crf: int, backend: str) -> tuple[float, float]:
        with tempfile.TemporaryDirectory(prefix="vmaftune-fast-verify-") as td:
            tmpdir = Path(td)
            dist = tmpdir / "dist.mp4"

            width, height, fps = _probe_video_geometry(src, cfg, subprocess.run)  # type: ignore[arg-type]
            if width == 0 or height == 0 or fps == 0.0:
                raise RuntimeError(f"fast encode_runner: ffprobe failed for {src}")

            is_container = src.suffix.lower() not in {".yuv", ".y4m", ""}
            enc_req = EncodeRequest(
                source=src,
                width=width,
                height=height,
                pix_fmt=pix_fmt,
                framerate=fps,
                encoder=encoder,
                preset=preset,
                crf=crf,
                output=dist,
                source_is_container=is_container,
            )
            enc_result = run_encode(enc_req, ffmpeg_bin=ffmpeg_bin)
            if enc_result.exit_status != 0 or not dist.exists():
                raise RuntimeError(
                    f"fast encode_runner: encode failed (CRF {crf}): "
                    f"{enc_result.stderr_tail[-300:]}"
                )

            # Approximate duration from frame count; good enough for kbps.
            size_bytes = dist.stat().st_size
            score_req = ScoreRequest(
                reference=src,
                distorted=dist,
                width=width,
                height=height,
                pix_fmt=pix_fmt,
            )
            score_result = run_score(
                score_req,
                vmaf_bin=vmaf_bin,
                backend=backend if backend != "auto" else None,
            )
            if score_result.exit_status != 0:
                raise RuntimeError(
                    f"fast encode_runner: score failed: {score_result.stderr_tail[-300:]}"
                )

            # Duration from encoder stats if available, else encode time proxy.
            enc_duration_s = enc_result.encode_time_ms / 1000.0 or 1.0
            kbps = bitrate_kbps(size_bytes, enc_duration_s)
            return kbps, score_result.vmaf_score

    return _run


def fast_recommend(
    src: Path | None,
    target_vmaf: float,
    encoder: str = "libx264",
    time_budget_s: int = 300,
    crf_range: tuple[int, int] = (DEFAULT_CRF_LO, DEFAULT_CRF_HI),
    n_trials: int | None = None,
    smoke: bool = False,
    predictor: Callable[[int], TrialSample] | None = None,
    sample_extractor: Callable[[Path, int, str], tuple[list[float], float]] | None = None,
    encode_runner: Callable[[Path, str, int, str], tuple[float, float]] | None = None,
    proxy_tolerance: float = DEFAULT_PROXY_TOLERANCE,
) -> dict[str, Any]:
    """Return a fast-path CRF recommendation for ``src`` at ``target_vmaf``.

    Production flow (``smoke=False``):

    1. Build a CRF→TrialSample predictor backed by ``fr_regressor_v2``
       (via :func:`_proxy_score`) and the injected ``sample_extractor``.
    2. Run :func:`_run_tpe` to converge on a recommended CRF.
    3. Run :func:`_gpu_verify` for a single real encode+score pass at
       the chosen CRF (proxy alone never wins).
    4. Report the proxy score, the verify score, and the absolute gap;
       flag OOD when the gap exceeds ``proxy_tolerance``.

    Smoke flow (``smoke=True``): synthetic CRF→VMAF curve, no proxy, no
    encode, no verify. Kept as the CI-friendly entry point.

    Parameters
    ----------
    src
        Path to the source video. ``None`` only in smoke mode.
    target_vmaf
        Quality target on the standard VMAF [0, 100] scale.
    encoder
        Codec adapter name (must be in ``ENCODER_VOCAB_V2`` for the
        production proxy path).
    time_budget_s
        Soft wall-clock budget for Optuna's TPE loop. Optuna stops
        scheduling new trials after the timeout; an in-flight trial is
        allowed to finish so probe encodes are not interrupted midway.
    crf_range
        ``(lo, hi)`` inclusive CRF search range.
    n_trials
        Number of TPE trials. Defaults to :data:`PROD_N_TRIALS` in
        production mode, :data:`SMOKE_N_TRIALS` in smoke mode.
    smoke
        Use the deterministic mock predictor (no ffmpeg / no ONNX /
        no GPU verify).
    predictor
        Optional override for the ``crf -> TrialSample`` callable.
        When supplied, both ``sample_extractor`` and the v2 proxy seam
        are bypassed. The verify pass still runs unless ``smoke=True``.
    sample_extractor
        Production seam — takes ``(src, crf, encoder)`` and returns
        ``(canonical_6_features, observed_kbps)``. Defaults to the
        encode-extract pipeline backed by ffmpeg + libvmaf JSON.
    encode_runner
        Production seam — takes ``(src, encoder, crf, backend)`` and
        returns ``(observed_kbps, vmaf_score)`` for the verify pass.
    proxy_tolerance
        VMAF gap above which the result is flagged OOD. The CLI exit
        code reflects this; in-process callers read
        ``proxy_verify_gap`` from the result dict.

    Returns
    -------
    dict
        Serialisable result; see :class:`FastRecommendResult`.

    Raises
    ------
    RuntimeError
        Optuna missing (install ``vmaf-tune[fast]``).
    ValueError
        Invalid in-process argument, such as ``src=None`` in production
        mode or a non-positive ``time_budget_s``.
    """
    _require_optuna()

    effective_n_trials = (
        n_trials if n_trials is not None else (SMOKE_N_TRIALS if smoke else PROD_N_TRIALS)
    )

    if smoke:
        chosen_predictor = predictor or _smoke_predictor
        recommended_crf, predicted_vmaf, predicted_kbps, completed_trials = _run_tpe(
            target_vmaf=target_vmaf,
            predictor=chosen_predictor,
            crf_range=crf_range,
            n_trials=effective_n_trials,
            time_budget_s=time_budget_s,
        )
        result = FastRecommendResult(
            encoder=encoder,
            target_vmaf=float(target_vmaf),
            recommended_crf=recommended_crf,
            predicted_vmaf=predicted_vmaf,
            predicted_kbps=predicted_kbps,
            n_trials=completed_trials,
            smoke=True,
            notes=(
                "smoke mode — synthetic predictor; no ffmpeg / ONNX / GPU. "
                "See ADR-0276 + ADR-0304 + Research-0076 for the production path."
            ),
            verify_vmaf=None,
            proxy_verify_gap=None,
        )
        return result.to_dict()

    # Production path.
    if src is None:
        raise ValueError(
            "vmaf-tune fast production mode requires a source path. "
            "Use smoke=True for the synthetic pipeline."
        )

    # Select the scoring backend once; forward it to both the TPE proxy
    # extractor and the GPU verify pass so all scoring — proxy trials +
    # the final verify encode — uses the same backend. ADR-0498 follow-
    # up #7: previously the sample extractor always defaulted to CPU
    # even when a GPU was available.
    from vmaftune.score_backend import select_backend as _select_backend

    _prod_backend = _select_backend(prefer="auto")

    if predictor is None:
        # Build the v2-proxy-backed predictor from the production
        # encode-extract sample seam, forwarding the selected backend
        # so each TPE trial scores on GPU when available.
        predictor = _build_prod_predictor(
            src=src,
            encoder=encoder,
            crf_range=crf_range,
            sample_extractor=sample_extractor,
            backend=_prod_backend,
        )

    recommended_crf, predicted_vmaf, predicted_kbps, completed_trials = _run_tpe(
        target_vmaf=target_vmaf,
        predictor=predictor,
        crf_range=crf_range,
        n_trials=effective_n_trials,
        time_budget_s=time_budget_s,
    )

    # Single GPU verify pass — mandatory; proxy alone never wins.
    verify_vmaf = _gpu_verify(
        src=src,
        encoder=encoder,
        crf=recommended_crf,
        encode_runner=encode_runner,
    )
    proxy_verify_gap = abs(predicted_vmaf - verify_vmaf)
    ood_flag = proxy_verify_gap > proxy_tolerance

    notes = (
        f"production: TPE over {effective_n_trials} trials with v2 proxy; "
        f"GPU verify gap = {proxy_verify_gap:.3f} VMAF "
        f"(tolerance {proxy_tolerance:.2f})."
    )
    if ood_flag:
        notes += (
            " FLAG: proxy/verify gap exceeds tolerance — consider falling "
            "back to the slow Phase A grid (ADR-0276)."
        )

    result = FastRecommendResult(
        encoder=encoder,
        target_vmaf=float(target_vmaf),
        recommended_crf=recommended_crf,
        predicted_vmaf=predicted_vmaf,
        predicted_kbps=predicted_kbps,
        n_trials=completed_trials,
        smoke=False,
        notes=notes,
        verify_vmaf=float(verify_vmaf),
        proxy_verify_gap=float(proxy_verify_gap),
    )
    return result.to_dict()


__all__ = [
    "DEFAULT_CRF_HI",
    "DEFAULT_CRF_LO",
    "DEFAULT_PROXY_TOLERANCE",
    "PROD_N_TRIALS",
    "SAMPLE_CHUNK_SECONDS",
    "SMOKE_N_TRIALS",
    "FastRecommendResult",
    "TrialSample",
    "_build_production_encode_runner",
    "_build_production_sample_extractor",
    "fast_recommend",
]
