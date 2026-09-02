# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Pelorus deband prefilter autotune (control-plane workstream D2).

Drives a VMAF-in-the-loop joint search over the Pelorus deband filter's
strength knobs (the 10-knob frozen contract, ADR-0110) **and** the
encoder's CRF axis. Each TPE trial:

1. proposes a deband parameter dict + a CRF,
2. runs ``[pelorus_deband_vulkan=...] -> HW encode -> VMAF score`` for
   that proposal (via an injected probe callable), and
3. feeds the achieved VMAF + bitrate back to Optuna.

The objective minimises ``|achieved_vmaf - target| + λ·kbps`` so the
search converges on the lowest-bitrate deband+CRF combination that hits
the target VMAF — exactly the framing in
:mod:`vmaftune.fast._objective_factory`, lifted to a multi-dimensional
search space.

vmafx is **Vulkan-free**: it never runs the Vulkan filter itself. The
adapter (:mod:`vmaftune.filter_adapters.pelorus_deband`) only emits the
``-vf`` fragment string; the live probe path is gated behind a
"pelorus filter not available" check (:func:`pelorus_filter_available`)
because the deband ffmpeg filter must be present in the ffmpeg build for
a real encode. Tests inject a fake probe and never touch ffmpeg.

Search engine: reuses the same Optuna ``TPESampler`` study the fast-path
(ADR-0276 / ADR-0304) uses. We deliberately do not reinvent the sampler;
this module only constructs the joint search space and the objective.
See ADR-1116 for the autotune-prefilter decision.
"""

from __future__ import annotations

import dataclasses
import math
import subprocess
from collections.abc import Callable, Mapping
from pathlib import Path
from typing import Any

from .filter_adapters import FilterAdapter, get_filter_adapter

# Optuna is the optional ``[fast]`` extra (same gate as fast.py).
try:  # pragma: no cover - import-guarded
    import optuna  # type: ignore[import-not-found]

    _OPTUNA_AVAILABLE = True
except ImportError:  # pragma: no cover - import-guarded
    optuna = None  # type: ignore[assignment]
    _OPTUNA_AVAILABLE = False


# Default CRF search window. The codec adapter's own ``quality_range``
# overrides this in the production path; the constants give the search
# a sane default + are reused by the CLI for ``--crf-min`` / ``--crf-max``
# help text. Matches the fast-path window (ADR-0276) for consistency.
DEFAULT_CRF_LO: int = 18
DEFAULT_CRF_HI: int = 40

# TPE trial budget. The joint search space is wider than the fast-path's
# single CRF axis (10 deband knobs + CRF = 11 dimensions), so the
# default trial count is higher; TPE still converges well under the
# soft time budget on a realistic probe-encode cost.
DEFAULT_N_TRIALS: int = 60

# Smoke-mode trial count — exercises the joint search wiring end-to-end
# with the synthetic probe, no ffmpeg / Vulkan / GPU.
SMOKE_N_TRIALS: int = 40

# Bitrate tie-breaker weight in the objective. Small relative to the
# quality term so the search primarily hits the VMAF target; ties break
# toward lower bitrate. Mirrors fast.py's bitrate_weight.
_BITRATE_WEIGHT: float = 1.0e-4

#: The probe seam: ``(deband_params, crf) -> ProbeResult``. Runs the
#: deband -> encode -> score loop for one proposal. Production builds it
#: from ffmpeg + libvmaf; tests inject a deterministic fake.
ProbeFn = Callable[[Mapping[str, float], int], "ProbeResult"]


class PelorusFilterUnavailableError(RuntimeError):
    """Raised when a live prefilter run is attempted without the filter.

    The Pelorus deband filter is a Vulkan ffmpeg filter that must be
    compiled into the ffmpeg build. vmafx only emits the ``-vf`` string;
    it cannot run the loop unless ``ffmpeg -filters`` lists
    ``pelorus_deband_vulkan``. This error gates the live path so the
    failure is a clear actionable message rather than an opaque ffmpeg
    "No such filter" stderr dump.
    """


@dataclasses.dataclass(frozen=True)
class ProbeResult:
    """Outcome of one ``deband -> encode -> score`` probe.

    ``vmaf`` is the achieved pooled VMAF for the (deband, crf) proposal;
    ``kbps`` is the observed output bitrate. ``vf_fragment`` is the exact
    ``-vf`` string the probe ran, retained for the per-probe audit trail.
    """

    vmaf: float
    kbps: float
    vf_fragment: str


@dataclasses.dataclass(frozen=True)
class ProbeRecord:
    """One recorded TPE trial for the per-probe report."""

    trial: int
    crf: int
    deband_params: dict[str, float]
    vf_fragment: str
    vmaf: float
    kbps: float
    objective: float


@dataclasses.dataclass(frozen=True)
class PrefilterResult:
    """Joint deband + CRF recommendation.

    ``recommended_deband`` is the winning knob dict (only the swept
    knobs; omitted knobs use the filter default). ``recommended_vf``
    is the emitted fragment for that dict, ready to splice into an
    ffmpeg command. ``probes`` carries every TPE trial's per-probe VMAF.
    """

    filter_name: str
    encoder: str
    target_vmaf: float
    recommended_crf: int
    recommended_deband: dict[str, float]
    recommended_vf: str
    achieved_vmaf: float
    achieved_kbps: float
    n_trials: int
    smoke: bool
    probes: tuple[ProbeRecord, ...] = ()
    notes: str = ""

    def to_dict(self) -> dict[str, Any]:
        payload = dataclasses.asdict(self)
        payload["probes"] = [dataclasses.asdict(p) for p in self.probes]
        return payload


def _require_optuna() -> None:
    if not _OPTUNA_AVAILABLE:
        raise RuntimeError(
            "vmaf-tune prefilter requires Optuna. Install with: "
            "pip install 'vmaf-tune[fast]'  (see docs/usage/vmaf-tune.md)."
        )


def pelorus_filter_available(
    ffmpeg_bin: str = "ffmpeg",
    *,
    filter_name: str = "pelorus_deband_vulkan",
    runner: Callable[..., subprocess.CompletedProcess[str]] | None = None,
) -> bool:
    """Return True iff ``ffmpeg -filters`` lists the deband filter.

    The live prefilter loop needs the Vulkan deband filter compiled into
    the ffmpeg build. This probe runs ``ffmpeg -hide_banner -filters``
    and greps for ``filter_name``. ``runner`` is a test seam defaulting
    to :func:`subprocess.run`; on any failure (ffmpeg missing, non-zero
    exit) the function returns False rather than raising — the caller
    turns that into :class:`PelorusFilterUnavailableError` with context.
    """
    if runner is None:
        runner = subprocess.run
    try:
        proc = runner(
            [ffmpeg_bin, "-hide_banner", "-filters"],
            capture_output=True,
            text=True,
            check=False,
        )
    except (OSError, ValueError):
        return False
    if proc.returncode != 0:
        return False
    return filter_name in (proc.stdout or "")


def build_search_space(
    adapter: FilterAdapter,
    *,
    crf_range: tuple[int, int],
    knobs: tuple[str, ...] | None = None,
) -> dict[str, tuple[str, float, float]]:
    """Construct the joint TPE search space for ``adapter`` + CRF.

    Returns a dict mapping each search dimension to a
    ``(kind, lo, hi)`` triple, where ``kind`` is ``"int"`` / ``"float"``
    / ``"bool"`` / ``"enum"`` for deband knobs and ``"int"`` for the
    synthetic ``"crf"`` dimension. The space is built straight from the
    adapter's frozen knob table (so it can never drift from the
    contract) plus the CRF axis.

    ``knobs`` optionally restricts the swept deband dimensions to a
    subset (the rest stay at the filter default); ``None`` sweeps all 10
    contract knobs. Unknown names raise ``KeyError`` via ``adapter.knob``.
    The CRF range is validated (``lo <= hi``, both >= 0).
    """
    crf_lo, crf_hi = crf_range
    if crf_lo < 0 or crf_hi < crf_lo:
        raise ValueError(f"invalid crf_range {crf_range!r}: need 0 <= lo <= hi")

    selected = adapter.knobs if knobs is None else tuple(adapter.knob(n) for n in knobs)
    space: dict[str, tuple[str, float, float]] = {}
    for knob in selected:
        space[knob.name] = (knob.kind, knob.lo, knob.hi)
    # CRF is an ordinal-integer search dimension joined to the deband
    # knobs — one Optuna study optimises both simultaneously.
    space["crf"] = ("int", float(crf_lo), float(crf_hi))
    return space


def _suggest_value(trial: Any, name: str, spec: tuple[str, float, float]) -> float:
    """Map one search dimension onto an Optuna ``suggest_*`` call.

    Integral dimensions (``int`` / ``bool`` / ``enum``) use
    ``suggest_int`` so TPE treats them as ordinal — per the contract,
    ``range`` is ordinal not continuous, and ``dither`` / ``dynamic`` /
    ``protect`` are discrete. ``float`` dimensions use ``suggest_float``
    over the contract's continuous range.
    """
    kind, lo, hi = spec
    if kind in ("int", "bool", "enum"):
        return float(trial.suggest_int(name, int(lo), int(hi)))
    return float(trial.suggest_float(name, lo, hi))


def _smoke_probe_factory(target_vmaf: float, adapter: FilterAdapter) -> ProbeFn:
    """Deterministic probe mimicking a deband+CRF -> VMAF surface.

    Higher CRF lowers VMAF and bitrate; debanding with moderate grain
    nudges VMAF slightly up near the target. The surface is smooth and
    has a clear optimum so the joint TPE search demonstrably converges
    without any ffmpeg / Vulkan / GPU. Used only by smoke mode.
    """

    def _probe(deband: Mapping[str, float], crf: int) -> ProbeResult:
        # CRF dominates: VMAF ~99 at CRF 18, ~78 at CRF 40 on a smooth taper.
        crf_norm = (crf - 18) / 22.0
        vmaf = 99.0 - 21.0 * max(0.0, crf_norm)
        # Mild deband bonus: moderate luma grain (~0.006) recovers a
        # little perceptual quality on banded content; too much hurts.
        grainy = float(deband.get("grainy", 0.006))
        vmaf += 1.5 * math.exp(-((grainy - 0.006) ** 2) / (2 * 0.01**2)) - 0.75
        kbps = 9000.0 * math.exp(-2.8 * max(0.0, crf_norm)) + 120.0
        frag = adapter.vf_fragment(deband)
        return ProbeResult(vmaf=vmaf, kbps=kbps, vf_fragment=frag)

    return _probe


def _run_joint_tpe(
    *,
    target_vmaf: float,
    adapter: FilterAdapter,
    probe: ProbeFn,
    search_space: dict[str, tuple[str, float, float]],
    n_trials: int,
    time_budget_s: float | None,
    seed: int = 0,
) -> tuple[int, dict[str, float], ProbeResult, tuple[ProbeRecord, ...]]:
    """Run the joint deband+CRF TPE search; return the winner + probe log.

    Reuses Optuna's ``TPESampler`` (the same engine fast.py uses). The
    objective is ``|achieved_vmaf - target| + λ·kbps``. Returns
    ``(best_crf, best_deband_params, best_probe, all_probe_records)``.
    """
    if time_budget_s is not None and time_budget_s <= 0.0:
        raise ValueError(f"time_budget_s must be > 0 when set; got {time_budget_s!r}")
    assert optuna is not None, "optuna not installed; call _require_optuna() first"

    records: list[ProbeRecord] = []

    def _objective(trial: Any) -> float:
        deband: dict[str, float] = {}
        crf = 0
        for name, spec in search_space.items():
            value = _suggest_value(trial, name, spec)
            if name == "crf":
                crf = int(value)
            else:
                deband[name] = value
        result = probe(deband, crf)
        quality_gap = abs(result.vmaf - target_vmaf)
        objective = float(quality_gap + _BITRATE_WEIGHT * result.kbps)
        trial.set_user_attr("vmaf", result.vmaf)
        trial.set_user_attr("kbps", result.kbps)
        records.append(
            ProbeRecord(
                trial=trial.number,
                crf=crf,
                deband_params=dict(deband),
                vf_fragment=result.vf_fragment,
                vmaf=result.vmaf,
                kbps=result.kbps,
                objective=objective,
            )
        )
        return objective

    optuna.logging.set_verbosity(optuna.logging.WARNING)
    study = optuna.create_study(
        direction="minimize",
        sampler=optuna.samplers.TPESampler(seed=seed),
    )
    study.optimize(
        _objective,
        n_trials=n_trials,
        timeout=float(time_budget_s) if time_budget_s is not None else None,
        show_progress_bar=False,
    )

    best = study.best_trial
    best_crf = int(best.params["crf"])
    best_deband = {k: v for k, v in best.params.items() if k != "crf"}
    best_probe = ProbeResult(
        vmaf=float(best.user_attrs.get("vmaf", float("nan"))),
        kbps=float(best.user_attrs.get("kbps", float("nan"))),
        vf_fragment=adapter.vf_fragment(best_deband),
    )
    return best_crf, best_deband, best_probe, tuple(records)


def recommend_prefilter(
    *,
    src: Path | None,
    target_vmaf: float,
    encoder: str = "libx264",
    filter_name: str = "pelorus_deband",
    crf_range: tuple[int, int] = (DEFAULT_CRF_LO, DEFAULT_CRF_HI),
    sweep_knobs: tuple[str, ...] | None = None,
    n_trials: int | None = None,
    time_budget_s: float | None = 600.0,
    smoke: bool = False,
    probe: ProbeFn | None = None,
    seed: int = 0,
) -> dict[str, Any]:
    """Recommend joint deband strengths + CRF for ``src`` at ``target_vmaf``.

    The search optimises the Pelorus deband knob space (the frozen
    contract, ADR-0110) **and** the CRF axis in one Optuna TPE study,
    returning the lowest-bitrate combination that hits the VMAF target.

    Production flow (``smoke=False``): the caller must inject ``probe``
    (the ``deband -> encode -> score`` loop) — this module does not run
    ffmpeg itself, and the live loop requires the Pelorus Vulkan filter
    in the ffmpeg build (gated by :func:`pelorus_filter_available`,
    enforced by the CLI handler before this function is reached).

    Smoke flow (``smoke=True``): a synthetic deband+CRF surface drives
    the joint search end-to-end with no ffmpeg / Vulkan / GPU.

    Parameters
    ----------
    src
        Source video path. ``None`` only in smoke mode (no encode runs).
    target_vmaf
        Quality target on the standard VMAF [0, 100] scale.
    encoder
        Codec adapter name (reused from ``codec_adapters``); recorded in
        the result and forwarded to the probe.
    filter_name
        Registered filter-adapter name (default ``pelorus_deband``).
    crf_range
        Inclusive ``(lo, hi)`` CRF search range.
    sweep_knobs
        Optional subset of deband knob names to sweep; ``None`` sweeps
        all 10 contract knobs. The rest stay at the filter default.
    n_trials
        TPE trial budget. Defaults to :data:`DEFAULT_N_TRIALS`
        (production) / :data:`SMOKE_N_TRIALS` (smoke).
    time_budget_s
        Soft wall-clock cap for the Optuna loop.
    smoke
        Use the synthetic probe (no ffmpeg / Vulkan / GPU).
    probe
        Production seam — ``(deband_params, crf) -> ProbeResult``.
        Required when ``smoke=False`` and no synthetic probe is wanted.
    seed
        TPE sampler seed (default 0) for reproducible searches.

    Returns
    -------
    dict
        Serialisable :class:`PrefilterResult`.

    Raises
    ------
    RuntimeError
        Optuna missing (install ``vmaf-tune[fast]``).
    ValueError
        Invalid argument (``src=None`` in production without a probe,
        non-positive budget, bad CRF range, unknown knob name).
    """
    _require_optuna()

    adapter = get_filter_adapter(filter_name)
    search_space = build_search_space(adapter, crf_range=crf_range, knobs=sweep_knobs)

    effective_n_trials = (
        n_trials if n_trials is not None else (SMOKE_N_TRIALS if smoke else DEFAULT_N_TRIALS)
    )

    if smoke:
        chosen_probe = probe or _smoke_probe_factory(target_vmaf, adapter)
    else:
        if probe is None:
            raise ValueError(
                "recommend_prefilter production mode requires an injected "
                "probe callable (deband, crf) -> ProbeResult. The live "
                "deband->encode->score loop is built by the CLI handler "
                "from ffmpeg + libvmaf and gated on pelorus_filter_available()."
            )
        if src is None:
            raise ValueError(
                "recommend_prefilter production mode requires a source path "
                "(src=None is only valid for smoke=True)."
            )
        chosen_probe = probe

    best_crf, best_deband, best_probe, records = _run_joint_tpe(
        target_vmaf=target_vmaf,
        adapter=adapter,
        probe=chosen_probe,
        search_space=search_space,
        n_trials=effective_n_trials,
        time_budget_s=time_budget_s,
        seed=seed,
    )

    swept = tuple(n for n in search_space if n != "crf")
    if smoke:
        notes = (
            "smoke mode — synthetic deband+CRF surface; no ffmpeg / Vulkan / "
            "GPU. Joint TPE over deband knobs + CRF (ADR-1116 / ADR-0106). "
            f"Swept knobs: {', '.join(swept)}."
        )
    else:
        notes = (
            f"production: joint TPE over {len(swept)} deband knobs + CRF, "
            f"{len(records)} probes against the pelorus_deband_vulkan filter "
            "(ADR-0110 contract). VMAF is the oracle; lowest-bitrate hit wins."
        )

    result = PrefilterResult(
        filter_name=adapter.filter_name,
        encoder=encoder,
        target_vmaf=float(target_vmaf),
        recommended_crf=best_crf,
        recommended_deband=best_deband,
        recommended_vf=best_probe.vf_fragment,
        achieved_vmaf=float(best_probe.vmaf),
        achieved_kbps=float(best_probe.kbps),
        n_trials=len(records),
        smoke=smoke,
        probes=records,
        notes=notes,
    )
    return result.to_dict()


__all__ = [
    "DEFAULT_CRF_HI",
    "DEFAULT_CRF_LO",
    "DEFAULT_N_TRIALS",
    "SMOKE_N_TRIALS",
    "PelorusFilterUnavailableError",
    "PrefilterResult",
    "ProbeFn",
    "ProbeRecord",
    "ProbeResult",
    "build_search_space",
    "pelorus_filter_available",
    "recommend_prefilter",
]
