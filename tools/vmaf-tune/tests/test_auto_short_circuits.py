# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the seven F.2 short-circuit predicates (ADR-0364).

Each predicate is a pure function of (meta, plan_state); the tests
mock both inputs and assert the branch fires (or doesn't) at the
boundary. The driver-level smoke tests cover order-of-evaluation
determinism and the JSON metadata block.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.auto import (
    LADDER_MULTI_RUNG_HEIGHT,
    PHASE_D_DURATION_GATE_S,
    PHASE_D_SHOT_VARIANCE_GATE,
    SALIENCY_CONTENT_CLASSES,
    SHORT_CIRCUIT_PREDICATES,
    PlanState,
    ShortCircuit,
    SourceMeta,
    _probe_source_meta,
    _should_short_circuit_1_single_rung_ladder,
    _should_short_circuit_2_codec_pinned,
    _should_short_circuit_3_predictor_gospel,
    _should_short_circuit_4_skip_saliency,
    _should_short_circuit_5_sdr_skip,
    _should_short_circuit_6_sample_clip_propagate,
    _should_short_circuit_7_skip_per_shot,
    emit_plan_json,
    evaluate_short_circuits,
    pick_auto_winner,
    run_auto,
)


class _FakeCompleted:
    def __init__(self, returncode: int = 0, stdout: str = "", stderr: str = "") -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _auto_probe_runner(cmd, capture_output, text, check):
    del capture_output, text, check
    joined = " ".join(str(part) for part in cmd)
    if "format=duration" in joined:
        return _FakeCompleted(stdout=json.dumps({"format": {"duration": "456.5"}}))
    if "stream=width,height,r_frame_rate" in joined:
        return _FakeCompleted(
            stdout=json.dumps(
                {
                    "streams": [
                        {
                            "width": 3840,
                            "height": 2160,
                            "r_frame_rate": "24000/1001",
                        }
                    ]
                }
            )
        )
    if "stream=color_transfer" in joined:
        return _FakeCompleted(
            stdout=json.dumps(
                {
                    "streams": [
                        {
                            "color_transfer": "smpte2084",
                            "color_primaries": "bt2020",
                            "color_space": "bt2020nc",
                            "color_range": "tv",
                            "pix_fmt": "yuv420p10le",
                        }
                    ]
                }
            )
        )
    return _FakeCompleted(returncode=1, stderr=f"unexpected command: {joined}")


def _meta(**overrides) -> SourceMeta:
    base = {
        "height": 1080,
        "width": 1920,
        "is_hdr": False,
        "content_class": "live_action",
        "duration_s": 60.0,
        "shot_variance": 0.05,
        "sample_clip_seconds": 0.0,
    }
    base.update(overrides)
    return SourceMeta(**base)


def _state(**overrides) -> PlanState:
    base = {
        "target_vmaf": 93.0,
        "max_budget_kbps": 5000.0,
        "allow_codecs": ("libx264",),
        "user_pinned_codec": None,
        "predictor_verdict": None,
    }
    base.update(overrides)
    return PlanState(**base)


# ---------------------------------------------------------------------------
# Short-circuit #1 — single-rung ladder when meta.height < 2160.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("height", [240, 480, 720, 1080, 1440, 2159])
def test_sc1_fires_below_2160p(height: int) -> None:
    assert _should_short_circuit_1_single_rung_ladder(_meta(height=height), _state())


@pytest.mark.parametrize("height", [2160, 2161, 4320])
def test_sc1_does_not_fire_at_or_above_2160p(height: int) -> None:
    assert not _should_short_circuit_1_single_rung_ladder(_meta(height=height), _state())


def test_sc1_boundary_at_threshold() -> None:
    # 2159 fires (sub-4K); 2160 does not (multi-rung path).
    assert _should_short_circuit_1_single_rung_ladder(
        _meta(height=LADDER_MULTI_RUNG_HEIGHT - 1), _state()
    )
    assert not _should_short_circuit_1_single_rung_ladder(
        _meta(height=LADDER_MULTI_RUNG_HEIGHT), _state()
    )


# ---------------------------------------------------------------------------
# Short-circuit #2 — codec known / pinned.
# ---------------------------------------------------------------------------


def test_sc2_fires_when_allow_codecs_has_one_entry() -> None:
    assert _should_short_circuit_2_codec_pinned(_meta(), _state(allow_codecs=("libx264",)))


def test_sc2_fires_when_user_pinned_overrides_multi_codec_list() -> None:
    state = _state(allow_codecs=("libx264", "libx265"), user_pinned_codec="libx265")
    assert _should_short_circuit_2_codec_pinned(_meta(), state)


def test_sc2_does_not_fire_on_genuine_shortlist() -> None:
    state = _state(allow_codecs=("libx264", "libx265", "libsvtav1"))
    assert not _should_short_circuit_2_codec_pinned(_meta(), state)


def test_sc2_does_not_fire_on_empty_allow_list() -> None:
    # Empty list != "one entry"; downstream validation rejects empty.
    state = _state(allow_codecs=())
    assert not _should_short_circuit_2_codec_pinned(_meta(), state)


# ---------------------------------------------------------------------------
# Short-circuit #3 — predictor verdict GOSPEL.
# ---------------------------------------------------------------------------


def test_sc3_fires_on_gospel() -> None:
    assert _should_short_circuit_3_predictor_gospel(_meta(), _state(predictor_verdict="GOSPEL"))


@pytest.mark.parametrize("verdict", [None, "RECALIBRATE", "FALL_BACK", "LIKELY", "UNKNOWN"])
def test_sc3_does_not_fire_on_non_gospel(verdict) -> None:
    assert not _should_short_circuit_3_predictor_gospel(_meta(), _state(predictor_verdict=verdict))


# ---------------------------------------------------------------------------
# Short-circuit #4 — non-screen-content skips saliency.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("content_class", ["live_action", "photographic", "documentary", "sports"])
def test_sc4_fires_on_photographic_content(content_class: str) -> None:
    assert _should_short_circuit_4_skip_saliency(_meta(content_class=content_class), _state())


@pytest.mark.parametrize("content_class", sorted(SALIENCY_CONTENT_CLASSES))
def test_sc4_does_not_fire_on_saliency_eligible_content(content_class: str) -> None:
    assert not _should_short_circuit_4_skip_saliency(_meta(content_class=content_class), _state())


# ---------------------------------------------------------------------------
# Short-circuit #5 — SDR source skips HDR pipeline.
# ---------------------------------------------------------------------------


def test_sc5_fires_on_sdr() -> None:
    assert _should_short_circuit_5_sdr_skip(_meta(is_hdr=False), _state())


def test_sc5_does_not_fire_on_hdr() -> None:
    assert not _should_short_circuit_5_sdr_skip(_meta(is_hdr=True), _state())


# ---------------------------------------------------------------------------
# Short-circuit #6 — sample-clip propagation.
# ---------------------------------------------------------------------------


def test_sc6_fires_when_user_set_sample_clip() -> None:
    assert _should_short_circuit_6_sample_clip_propagate(_meta(sample_clip_seconds=10.0), _state())


def test_sc6_does_not_fire_at_zero() -> None:
    assert not _should_short_circuit_6_sample_clip_propagate(
        _meta(sample_clip_seconds=0.0), _state()
    )


def test_sc6_does_not_fire_on_negative_garbage() -> None:
    # Defensive: a negative value should not be treated as "user set".
    assert not _should_short_circuit_6_sample_clip_propagate(
        _meta(sample_clip_seconds=-1.0), _state()
    )


# ---------------------------------------------------------------------------
# Short-circuit #7 — duration AND shot-variance gate.
# ---------------------------------------------------------------------------


def test_sc7_fires_on_short_low_variance() -> None:
    assert _should_short_circuit_7_skip_per_shot(
        _meta(duration_s=60.0, shot_variance=0.05), _state()
    )


def test_sc7_requires_both_conditions_short_only_does_not_fire() -> None:
    # Short but high-variance — needs per-shot.
    assert not _should_short_circuit_7_skip_per_shot(
        _meta(duration_s=60.0, shot_variance=0.40), _state()
    )


def test_sc7_requires_both_conditions_low_var_only_does_not_fire() -> None:
    # Long but low-variance — still benefits from per-shot pass.
    assert not _should_short_circuit_7_skip_per_shot(
        _meta(duration_s=3600.0, shot_variance=0.05), _state()
    )


def test_sc7_does_not_fire_on_long_high_variance() -> None:
    assert not _should_short_circuit_7_skip_per_shot(
        _meta(duration_s=3600.0, shot_variance=0.40), _state()
    )


def test_sc7_threshold_boundaries() -> None:
    # Strictly less than the threshold for both axes.
    assert _should_short_circuit_7_skip_per_shot(
        _meta(
            duration_s=PHASE_D_DURATION_GATE_S - 1.0,
            shot_variance=PHASE_D_SHOT_VARIANCE_GATE - 0.001,
        ),
        _state(),
    )
    # At the duration boundary — does not fire (strict inequality).
    assert not _should_short_circuit_7_skip_per_shot(
        _meta(
            duration_s=PHASE_D_DURATION_GATE_S,
            shot_variance=PHASE_D_SHOT_VARIANCE_GATE - 0.001,
        ),
        _state(),
    )
    # At the variance boundary — does not fire.
    assert not _should_short_circuit_7_skip_per_shot(
        _meta(
            duration_s=PHASE_D_DURATION_GATE_S - 1.0,
            shot_variance=PHASE_D_SHOT_VARIANCE_GATE,
        ),
        _state(),
    )


# ---------------------------------------------------------------------------
# Order-of-evaluation determinism.
# ---------------------------------------------------------------------------


def test_short_circuit_predicates_in_canonical_order() -> None:
    # Asserts the public ordering of all predicates. The original seven
    # occupy positions 0–6; the F.1/F.2 additions (#8, #9, #10) are
    # appended at positions 7–9. Adding a new short-circuit MUST append,
    # never insert — earlier positions are part of the contract.
    all_scs = tuple(sc for sc, _ in SHORT_CIRCUIT_PREDICATES)
    assert all_scs[:7] == (
        ShortCircuit.LADDER_SINGLE_RUNG,
        ShortCircuit.CODEC_PINNED,
        ShortCircuit.PREDICTOR_GOSPEL,
        ShortCircuit.SKIP_SALIENCY,
        ShortCircuit.SDR_SKIP,
        ShortCircuit.SAMPLE_CLIP_PROPAGATE,
        ShortCircuit.SKIP_PER_SHOT,
    )
    assert all_scs[7] is ShortCircuit.LOW_COMPLEXITY
    assert all_scs[8] is ShortCircuit.BASELINE_MEETS_TARGET
    assert all_scs[9] is ShortCircuit.NO_TWO_PASS


def test_evaluate_short_circuits_idempotent() -> None:
    meta = _meta(height=1080, is_hdr=False, content_class="live_action")
    state = _state(allow_codecs=("libx264",), predictor_verdict="GOSPEL")
    first = evaluate_short_circuits(meta, state)
    second = evaluate_short_circuits(meta, state)
    assert first == second


def test_evaluate_short_circuits_full_fire() -> None:
    # Construct a meta + state that fires every short-circuit at once;
    # asserts no predicate shadows another (the recorded list contains
    # all ten IDs, in canonical order).
    meta = _meta(
        height=1080,
        is_hdr=False,
        content_class="live_action",
        duration_s=60.0,
        shot_variance=0.05,
        sample_clip_seconds=10.0,
        # F.1/F.2 additions: low-complexity score + baseline above target.
        complexity_score=10.0,
        baseline_vmaf=99.0,
    )
    state = _state(
        allow_codecs=("libx264",),
        user_pinned_codec=None,
        predictor_verdict="GOSPEL",
        # F.1/F.2 addition: adapter declares no two-pass support.
        adapter_supports_two_pass=False,
    )
    fired = evaluate_short_circuits(meta, state)
    assert fired == [sc.value for sc, _ in SHORT_CIRCUIT_PREDICATES]


def test_evaluate_short_circuits_none_fire_on_pessimistic_inputs() -> None:
    # 4K HDR animation source with multi-codec shortlist, no predictor
    # verdict, long duration, high shot variance, no sample clip.
    meta = _meta(
        height=4320,
        is_hdr=True,
        content_class="animation",
        duration_s=3600.0,
        shot_variance=0.40,
        sample_clip_seconds=0.0,
    )
    state = _state(
        allow_codecs=("libx264", "libx265", "libsvtav1"),
        user_pinned_codec=None,
        predictor_verdict="FALL_BACK",
    )
    fired = evaluate_short_circuits(meta, state)
    assert fired == []


# ---------------------------------------------------------------------------
# Driver-level smoke — JSON metadata block records firing short-circuits.
# ---------------------------------------------------------------------------


def test_run_auto_smoke_records_short_circuits() -> None:
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        user_pinned_codec=None,
        sample_clip_seconds=0.0,
        smoke=True,
    )
    fired = plan.metadata["short_circuits"]
    # Synthetic 1080p SDR live-action meta -> fires #1, #2, #3 (smoke
    # synthesises GOSPEL), #4 (live_action), #5 (SDR), #7 (short
    # low-variance). Sample-clip propagation is NOT fired because the
    # caller passed 0.
    assert "ladder-single-rung" in fired
    assert "codec-pinned" in fired
    assert "predictor-gospel" in fired
    assert "skip-saliency" in fired
    assert "sdr-skip" in fired
    assert "skip-per-shot" in fired
    assert "sample-clip-propagate" not in fired


def test_run_auto_smoke_emits_stable_json() -> None:
    plan = run_auto(
        src=Path("/tmp/x.yuv"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        sample_clip_seconds=10.0,
        smoke=True,
    )
    rendered = emit_plan_json(plan)
    payload = json.loads(rendered)
    assert "cells" in payload
    assert "metadata" in payload
    assert payload["metadata"]["short_circuits"]
    assert "sample-clip-propagate" in payload["metadata"]["short_circuits"]
    assert payload["metadata"]["winner"]["status"] == "budget_and_quality_met"
    assert payload["cells"][payload["metadata"]["winner"]["cell_index"]]["selected"] is True


def test_pick_auto_winner_prefers_lowest_bitrate_inside_quality_and_budget() -> None:
    cells = [
        {
            "rung": 1080,
            "codec": "libx264",
            "crf": 22,
            "estimated_vmaf": 94.0,
            "estimated_bitrate_kbps": 4700.0,
        },
        {
            "rung": 1080,
            "codec": "libx265",
            "crf": 24,
            "estimated_vmaf": 93.5,
            "estimated_bitrate_kbps": 3200.0,
        },
        {
            "rung": 720,
            "codec": "libx265",
            "crf": 28,
            "estimated_vmaf": 91.0,
            "estimated_bitrate_kbps": 1800.0,
        },
    ]

    winner = pick_auto_winner(cells, target_vmaf=93.0, max_budget_kbps=5000.0)

    assert winner["status"] == "budget_and_quality_met"
    assert winner["cell_index"] == 1
    assert winner["codec"] == "libx265"
    assert winner["budget_margin_kbps"] == pytest.approx(1800.0)


def test_pick_auto_winner_keeps_quality_gate_when_budget_is_exceeded() -> None:
    cells = [
        {
            "rung": 1080,
            "codec": "libx264",
            "crf": 22,
            "estimated_vmaf": 94.0,
            "estimated_bitrate_kbps": 6200.0,
        },
        {
            "rung": 1080,
            "codec": "libx265",
            "crf": 24,
            "estimated_vmaf": 93.2,
            "estimated_bitrate_kbps": 5400.0,
        },
        {
            "rung": 720,
            "codec": "libx265",
            "crf": 30,
            "estimated_vmaf": 91.0,
            "estimated_bitrate_kbps": 2000.0,
        },
    ]

    winner = pick_auto_winner(cells, target_vmaf=93.0, max_budget_kbps=5000.0)

    assert winner["status"] == "quality_met_budget_exceeded"
    assert winner["cell_index"] == 1
    assert winner["budget_margin_kbps"] == pytest.approx(-400.0)


def test_pick_auto_winner_returns_closest_quality_miss_when_target_unmet() -> None:
    cells = [
        {
            "rung": 720,
            "codec": "libx264",
            "crf": 28,
            "estimated_vmaf": 89.0,
            "estimated_bitrate_kbps": 1100.0,
        },
        {
            "rung": 1080,
            "codec": "libx265",
            "crf": 25,
            "estimated_vmaf": 92.5,
            "estimated_bitrate_kbps": 3400.0,
        },
    ]

    winner = pick_auto_winner(cells, target_vmaf=93.0, max_budget_kbps=5000.0)

    assert winner["status"] == "target_unmet"
    assert winner["cell_index"] == 1
    assert winner["quality_margin"] == pytest.approx(-0.5)


def test_probe_source_meta_reads_geometry_duration_and_hdr(tmp_path: Path) -> None:
    src = tmp_path / "hdr.mp4"
    src.write_bytes(b"\x00")
    meta, hdr_info = _probe_source_meta(
        src,
        sample_clip_seconds=12.0,
        runner=_auto_probe_runner,
    )
    assert meta.width == 3840
    assert meta.height == 2160
    assert meta.duration_s == pytest.approx(456.5)
    assert meta.sample_clip_seconds == pytest.approx(12.0)
    assert meta.is_hdr is True
    assert hdr_info is not None
    assert hdr_info.transfer == "pq"


def test_run_auto_non_smoke_uses_probe_runner_when_no_meta(tmp_path: Path) -> None:
    src = tmp_path / "hdr.mp4"
    src.write_bytes(b"\x00")
    plan = run_auto(
        src=src,
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=False,
        probe_runner=_auto_probe_runner,
    )
    assert plan.metadata["source_meta"]["width"] == 3840
    assert plan.metadata["source_meta"]["height"] == 2160
    assert plan.metadata["source_meta"]["duration_s"] == pytest.approx(456.5)
    assert plan.metadata["source_meta"]["is_hdr"] is True


def test_run_auto_non_smoke_uses_predictor_for_cell_estimates() -> None:
    meta = SourceMeta(
        height=1080,
        width=1920,
        is_hdr=False,
        content_class="live_action",
        duration_s=180.0,
        shot_variance=0.07,
        complexity_score=2500.0,
        baseline_vmaf=0.0,
    )
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=92.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=False,
        meta_override=meta,
    )
    cell = plan.cells[0]
    assert cell["prediction_source"] == "predictor"
    assert 0 <= cell["crf"] <= 51
    assert cell["crf"] != 23
    assert cell["estimated_vmaf"] >= 92.0
    assert cell["estimated_bitrate_kbps"] != 5000.0


def test_run_auto_4k_hdr_animation_does_not_short_circuit_inappropriately() -> None:
    # Pessimistic input — codec, saliency, HDR, sample-clip, per-shot
    # stages all proceed. Note that under F.4 the ``animation`` recipe
    # intentionally arms ``force_single_rung``, so
    # ``ladder-single-rung`` *does* fire for an animation source even
    # at 2160p — that's the recipe's documented behaviour. Use a
    # non-animation 4K HDR source to assert the multi-rung path.
    meta = SourceMeta(
        height=2160,
        width=3840,
        is_hdr=True,
        content_class="live_action_hdr",
        duration_s=7200.0,
        shot_variance=0.30,
        sample_clip_seconds=0.0,
    )
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=20000.0,
        allow_codecs=("libx264", "libx265", "libsvtav1"),
        smoke=True,
        meta_override=meta,
    )
    fired = plan.metadata["short_circuits"]
    assert "ladder-single-rung" not in fired
    assert "codec-pinned" not in fired
    # live_action_hdr is not in SALIENCY_CONTENT_CLASSES, so saliency
    # is skipped; that's the F.2 behaviour preserved under F.4.
    assert "sdr-skip" not in fired
    assert "skip-per-shot" not in fired
    assert "sample-clip-propagate" not in fired
    # GOSPEL still fires because smoke mode synthesises the verdict.
    assert "predictor-gospel" in fired


def test_run_auto_hdr_cells_use_codec_specific_dispatch() -> None:
    meta = SourceMeta(
        height=2160,
        width=3840,
        is_hdr=True,
        content_class="live_action_hdr",
        duration_s=7200.0,
        shot_variance=0.30,
        sample_clip_seconds=0.0,
    )
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=20000.0,
        allow_codecs=("libx264", "libx265", "libsvtav1"),
        smoke=True,
        meta_override=meta,
    )
    by_codec = {}
    for cell in plan.cells:
        by_codec.setdefault(cell["codec"], cell["hdr_args"])

    assert "-color_trc" in by_codec["libx264"]
    assert "-x265-params" in by_codec["libx265"]
    assert any("hdr10-opt=1" in arg for arg in by_codec["libx265"])
    assert "-svtav1-params" in by_codec["libsvtav1"]
    assert len({tuple(args) for args in by_codec.values()}) == 3


def test_run_auto_hdr_hardware_cells_are_not_left_unflagged() -> None:
    meta = SourceMeta(
        height=2160,
        width=3840,
        is_hdr=True,
        content_class="live_action_hdr",
        duration_s=7200.0,
        shot_variance=0.30,
        sample_clip_seconds=0.0,
    )
    codecs = (
        "av1_nvenc",
        "hevc_qsv",
        "av1_qsv",
        "hevc_amf",
        "av1_amf",
        "hevc_videotoolbox",
    )
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=20000.0,
        allow_codecs=codecs,
        smoke=True,
        meta_override=meta,
    )
    by_codec = {}
    for cell in plan.cells:
        by_codec.setdefault(cell["codec"], cell["hdr_args"])

    for codec in codecs:
        assert "-color_primaries" in by_codec[codec], codec
        assert by_codec[codec][by_codec[codec].index("-color_trc") + 1] == "smpte2084"

    for codec in ("av1_nvenc", "hevc_qsv", "av1_qsv", "hevc_amf", "av1_amf"):
        assert "-pix_fmt" in by_codec[codec], codec
        assert by_codec[codec][by_codec[codec].index("-pix_fmt") + 1] == "p010le"

    for codec in ("hevc_qsv", "hevc_amf", "hevc_videotoolbox"):
        assert "-profile:v" in by_codec[codec], codec
        assert by_codec[codec][by_codec[codec].index("-profile:v") + 1] == "main10"


def test_run_auto_does_not_dispatch_fast_subcommand() -> None:
    # Coordination with PR #467 — the Phase F driver must NOT invoke
    # the `fast` subcommand from inside its tree. We assert this by
    # verifying that vmaftune.fast is never imported as a side effect
    # of run_auto, and that no cell records a "fast" pipeline marker.
    import importlib
    import sys as _sys

    _sys.modules.pop("vmaftune.fast", None)
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
    )
    assert "vmaftune.fast" not in _sys.modules
    for cell in plan.cells:
        assert cell.get("pipeline") != "fast"
    # Sanity: importing fast directly still works (no breakage).
    importlib.import_module("vmaftune.fast")
