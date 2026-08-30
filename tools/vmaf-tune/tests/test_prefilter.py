# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Pelorus prefilter joint-search tests (ADR-1116 / pelorus ADR-0110).

Unit-level only — no live encode. Validates:

1. The joint search space is the 10 deband knobs + the CRF axis, built
   straight from the frozen contract.
2. The smoke joint TPE search converges toward the target.
3. An injected probe drives the joint search without ffmpeg / Vulkan /
   GPU, and the recommended deband+CRF round-trips into a valid -vf.
4. The live path is gated behind ``pelorus_filter_available`` and raises
   a clear error when no probe is supplied.
"""

from __future__ import annotations

import sys
from collections.abc import Mapping
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

optuna = pytest.importorskip("optuna")

from vmaftune.filter_adapters import get_filter_adapter
from vmaftune.prefilter import (
    DEFAULT_N_TRIALS,
    SMOKE_N_TRIALS,
    ProbeResult,
    build_search_space,
    pelorus_filter_available,
    recommend_prefilter,
)

# ---------------------------------------------------------------------------
# Search-space construction.
# ---------------------------------------------------------------------------


def test_search_space_is_ten_knobs_plus_crf() -> None:
    adapter = get_filter_adapter("pelorus_deband")
    space = build_search_space(adapter, crf_range=(18, 40))
    # 10 deband knobs + the synthetic crf axis.
    assert len(space) == 11
    assert "crf" in space
    assert space["crf"] == ("int", 18.0, 40.0)
    # Each deband dim carries its (kind, lo, hi) from the contract.
    assert space["range"] == ("int", 1.0, 31.0)
    assert space["thry"] == ("float", 0.0, 0.25)
    assert space["dither"] == ("enum", 0.0, 2.0)
    assert space["dynamic"] == ("bool", 0.0, 1.0)


def test_search_space_subset_sweep() -> None:
    adapter = get_filter_adapter("pelorus_deband")
    space = build_search_space(adapter, crf_range=(20, 30), knobs=("range", "grainy"))
    assert set(space) == {"range", "grainy", "crf"}


def test_search_space_unknown_knob_raises() -> None:
    adapter = get_filter_adapter("pelorus_deband")
    with pytest.raises(KeyError):
        build_search_space(adapter, crf_range=(18, 40), knobs=("sharpness",))


def test_search_space_bad_crf_range_raises() -> None:
    adapter = get_filter_adapter("pelorus_deband")
    with pytest.raises(ValueError, match="invalid crf_range"):
        build_search_space(adapter, crf_range=(40, 18))


# ---------------------------------------------------------------------------
# Joint smoke search.
# ---------------------------------------------------------------------------


def test_smoke_joint_search_converges() -> None:
    result = recommend_prefilter(
        src=None,
        target_vmaf=92.0,
        smoke=True,
        n_trials=SMOKE_N_TRIALS,
        time_budget_s=60.0,
    )
    assert result["smoke"] is True
    assert result["filter_name"] == "pelorus_deband_vulkan"
    assert 18 <= result["recommended_crf"] <= 40
    # The synthetic surface is smooth; TPE should land within ~2 VMAF.
    assert abs(result["achieved_vmaf"] - 92.0) < 2.5
    assert result["recommended_vf"].startswith("pelorus_deband_vulkan=")
    # Every probe is recorded with its per-probe VMAF.
    assert len(result["probes"]) == result["n_trials"]
    assert all("vmaf" in p and "vf_fragment" in p for p in result["probes"])


def test_smoke_recommended_deband_roundtrips_to_valid_vf() -> None:
    result = recommend_prefilter(src=None, target_vmaf=90.0, smoke=True, n_trials=20)
    adapter = get_filter_adapter("pelorus_deband")
    # The winning deband dict must validate against the frozen contract.
    adapter.validate(result["recommended_deband"])
    assert adapter.vf_fragment(result["recommended_deband"]) == result["recommended_vf"]


def test_smoke_subset_sweep_only_touches_requested_knobs() -> None:
    result = recommend_prefilter(
        src=None,
        target_vmaf=90.0,
        smoke=True,
        n_trials=20,
        sweep_knobs=("range", "grainy"),
    )
    assert set(result["recommended_deband"]) == {"range", "grainy"}


# ---------------------------------------------------------------------------
# Injected probe (closes the loop without ffmpeg).
# ---------------------------------------------------------------------------


def test_injected_probe_drives_joint_search() -> None:
    adapter = get_filter_adapter("pelorus_deband")
    seen: list[tuple[dict, int]] = []

    def _fake_probe(deband: Mapping[str, float], crf: int) -> ProbeResult:
        seen.append((dict(deband), crf))
        # CRF dominates VMAF; deband is a small perturbation.
        vmaf = 100.0 - (crf - 18) * 0.9
        return ProbeResult(
            vmaf=vmaf, kbps=float(8000 - crf * 100), vf_fragment=adapter.vf_fragment(deband)
        )

    result = recommend_prefilter(
        src=Path("src.yuv"),
        target_vmaf=88.0,
        smoke=False,
        probe=_fake_probe,
        n_trials=15,
        crf_range=(18, 40),
        time_budget_s=60.0,
    )
    assert len(seen) > 0
    assert result["smoke"] is False
    assert result["n_trials"] == len(seen)
    # Each probe saw all 10 deband knobs + a CRF.
    deband0, crf0 = seen[0]
    assert set(deband0) == {k.name for k in adapter.knobs}
    assert 18 <= crf0 <= 40


def test_production_default_trial_count() -> None:
    def _flat_probe(deband: Mapping[str, float], crf: int) -> ProbeResult:
        return ProbeResult(vmaf=90.0, kbps=2000.0, vf_fragment="pelorus_deband_vulkan")

    result = recommend_prefilter(
        src=Path("src.yuv"),
        target_vmaf=90.0,
        smoke=False,
        probe=_flat_probe,
    )
    assert result["n_trials"] == DEFAULT_N_TRIALS


# ---------------------------------------------------------------------------
# Live-path gating.
# ---------------------------------------------------------------------------


def test_production_without_probe_raises() -> None:
    with pytest.raises(ValueError, match="requires an injected probe"):
        recommend_prefilter(src=Path("src.yuv"), target_vmaf=90.0, smoke=False)


def test_production_without_src_raises() -> None:
    def _probe(deband: Mapping[str, float], crf: int) -> ProbeResult:
        return ProbeResult(vmaf=90.0, kbps=2000.0, vf_fragment="x")

    with pytest.raises(ValueError, match="requires a source path"):
        recommend_prefilter(src=None, target_vmaf=90.0, smoke=False, probe=_probe)


def test_pelorus_filter_available_detects_filter() -> None:
    import subprocess

    def _runner_present(cmd, **_kw):
        return subprocess.CompletedProcess(
            cmd, 0, stdout="... pelorus_deband_vulkan  Smart deband ...\n", stderr=""
        )

    def _runner_absent(cmd, **_kw):
        return subprocess.CompletedProcess(cmd, 0, stdout="... scale  Scale video ...\n", stderr="")

    def _runner_fails(cmd, **_kw):
        return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="boom")

    assert pelorus_filter_available("ffmpeg", runner=_runner_present) is True
    assert pelorus_filter_available("ffmpeg", runner=_runner_absent) is False
    assert pelorus_filter_available("ffmpeg", runner=_runner_fails) is False


def test_pelorus_filter_available_handles_missing_ffmpeg() -> None:
    def _runner_oserror(cmd, **_kw):
        raise FileNotFoundError("ffmpeg not on PATH")

    assert pelorus_filter_available("ffmpeg", runner=_runner_oserror) is False
