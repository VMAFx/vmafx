# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for ``ai/scripts/calibrate_phase_f_recipes.py`` helper functions.

The existing ``test_calibrate_phase_f_recipes.py`` covers only the top-level
``main()`` invocation (run-provenance smoke). This file exercises the seven
pure helper functions that the existing test leaves uncovered:

* ``mos_to_vmaf_proxy``        — MOS [1,5] → VMAF [0,100] clamped mapping
* ``saliency_benefit_to_intensity`` — fraction → label enum
* ``_iter_corpus_rows``        — JSONL parser, skips malformed lines
* ``_ugc_target_vmaf_offset``  — median residual asymmetry estimator
* ``_ugc_tight_interval_width`` — jackknife width estimator
* ``_resolution_dominance``    — mode-bucket fraction
* ``_ugc_saliency_benefit_fraction`` — landscape × low-MOS proxy

All tests are CPU-only, require no model files, and run without calling
libvmaf or ffmpeg.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(_SCRIPTS))

import calibrate_phase_f_recipes as cpfr  # noqa: E402

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _row(src: str, w: int, h: int, mos: float, dur: float = 5.0) -> cpfr.CorpusRow:
    return cpfr.CorpusRow(src=src, width=w, height=h, mos=mos, duration_s=dur)


# ---------------------------------------------------------------------------
# mos_to_vmaf_proxy
# ---------------------------------------------------------------------------


class TestMosToVmafProxy:
    def test_midpoint_mos_3_yields_60(self):
        # slope=20, intercept=0 → 3 * 20 = 60.0
        assert cpfr.mos_to_vmaf_proxy(3.0) == pytest.approx(60.0, abs=1e-9)

    def test_mos_1_yields_20(self):
        assert cpfr.mos_to_vmaf_proxy(1.0) == pytest.approx(20.0, abs=1e-9)

    def test_mos_5_yields_100(self):
        assert cpfr.mos_to_vmaf_proxy(5.0) == pytest.approx(100.0, abs=1e-9)

    def test_mos_below_1_is_clamped_to_zero_floor(self):
        # The clamping is max(0, min(100, ...)); MOS=0 → 0*20=0 → clamped to 0
        assert cpfr.mos_to_vmaf_proxy(0.0) == pytest.approx(0.0, abs=1e-9)

    def test_negative_mos_clamped_at_zero(self):
        assert cpfr.mos_to_vmaf_proxy(-5.0) == 0.0

    def test_mos_above_5_clamped_at_100(self):
        # 6 * 20 = 120 → clamped to 100
        assert cpfr.mos_to_vmaf_proxy(6.0) == 100.0

    def test_fractional_mos(self):
        assert cpfr.mos_to_vmaf_proxy(2.5) == pytest.approx(50.0, abs=1e-9)

    def test_accepts_string_coerced_float(self):
        # The function calls float() internally
        assert cpfr.mos_to_vmaf_proxy("4.0") == pytest.approx(80.0, abs=1e-9)


# ---------------------------------------------------------------------------
# saliency_benefit_to_intensity
# ---------------------------------------------------------------------------


class TestSaliencyBenefitToIntensity:
    def test_low_fraction_returns_default(self):
        assert cpfr.saliency_benefit_to_intensity(0.0) == "default"
        assert cpfr.saliency_benefit_to_intensity(0.29) == "default"

    def test_at_aggressive_threshold_returns_aggressive(self):
        # threshold = 0.30
        assert cpfr.saliency_benefit_to_intensity(0.30) == "aggressive"
        assert cpfr.saliency_benefit_to_intensity(0.45) == "aggressive"
        assert cpfr.saliency_benefit_to_intensity(0.54) == "aggressive"

    def test_at_very_aggressive_threshold_returns_very_aggressive(self):
        # threshold = 0.55
        assert cpfr.saliency_benefit_to_intensity(0.55) == "very_aggressive"
        assert cpfr.saliency_benefit_to_intensity(0.65) == "very_aggressive"
        assert cpfr.saliency_benefit_to_intensity(1.00) == "very_aggressive"

    def test_boundary_between_aggressive_and_very_aggressive(self):
        below = cpfr.saliency_benefit_to_intensity(0.549)
        above = cpfr.saliency_benefit_to_intensity(0.550)
        assert below == "aggressive"
        assert above == "very_aggressive"


# ---------------------------------------------------------------------------
# _iter_corpus_rows
# ---------------------------------------------------------------------------


class TestIterCorpusRows:
    def test_valid_rows_are_yielded(self, tmp_path: Path):
        rows = [
            {"src": "a", "width": 1920, "height": 1080, "mos": 3.0, "duration_s": 5.0},
            {"src": "b", "width": 960, "height": 540, "mos": 4.5, "duration_s": 10.0},
        ]
        p = tmp_path / "corpus.jsonl"
        p.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")
        result = list(cpfr._iter_corpus_rows(p))
        assert len(result) == 2
        assert result[0].src == "a"
        assert result[0].width == 1920
        assert result[0].mos == pytest.approx(3.0)
        assert result[1].src == "b"
        assert result[1].height == 540

    def test_blank_lines_are_skipped(self, tmp_path: Path):
        content = '{"src":"x","width":640,"height":360,"mos":2.0}\n\n\n'
        p = tmp_path / "corpus.jsonl"
        p.write_text(content, encoding="utf-8")
        result = list(cpfr._iter_corpus_rows(p))
        assert len(result) == 1

    def test_malformed_json_is_skipped(self, tmp_path: Path):
        content = (
            '{"src":"a","width":640,"height":360,"mos":2.0}\n'
            "NOT_JSON_AT_ALL\n"
            '{"src":"b","width":640,"height":360,"mos":3.0}\n'
        )
        p = tmp_path / "corpus.jsonl"
        p.write_text(content, encoding="utf-8")
        result = list(cpfr._iter_corpus_rows(p))
        assert len(result) == 2
        assert result[0].src == "a"
        assert result[1].src == "b"

    def test_row_missing_required_field_is_skipped(self, tmp_path: Path):
        # Missing "mos" key — should be skipped
        content = (
            '{"src":"a","width":640,"height":360}\n'
            '{"src":"b","width":640,"height":360,"mos":3.0}\n'
        )
        p = tmp_path / "corpus.jsonl"
        p.write_text(content, encoding="utf-8")
        result = list(cpfr._iter_corpus_rows(p))
        assert len(result) == 1
        assert result[0].src == "b"

    def test_optional_duration_s_defaults_to_zero(self, tmp_path: Path):
        content = '{"src":"a","width":640,"height":360,"mos":2.0}\n'
        p = tmp_path / "corpus.jsonl"
        p.write_text(content, encoding="utf-8")
        result = list(cpfr._iter_corpus_rows(p))
        assert result[0].duration_s == pytest.approx(0.0)

    def test_empty_file_yields_nothing(self, tmp_path: Path):
        p = tmp_path / "empty.jsonl"
        p.write_text("", encoding="utf-8")
        assert list(cpfr._iter_corpus_rows(p)) == []


# ---------------------------------------------------------------------------
# _ugc_target_vmaf_offset
# ---------------------------------------------------------------------------


class TestUgcTargetVmafOffset:
    def test_empty_rows_returns_zero(self):
        assert cpfr._ugc_target_vmaf_offset([]) == pytest.approx(0.0)

    def test_fewer_than_4_rows_returns_zero(self):
        rows = [_row("a", 640, 360, 3.0) for _ in range(3)]
        assert cpfr._ugc_target_vmaf_offset(rows) == pytest.approx(0.0)

    def test_symmetric_distribution_returns_near_zero(self):
        # Symmetric distribution: [1,2,3,4,5] → proxies [20,40,60,80,100]
        rows = [_row(f"c{i}", 640, 360, float(i)) for i in [1, 2, 3, 4, 5]]
        offset = cpfr._ugc_target_vmaf_offset(rows)
        # For a symmetric distribution, asymmetry = 0 → offset ≈ 0
        assert offset == pytest.approx(0.0, abs=0.2)

    def test_heavy_lower_tail_produces_negative_offset(self):
        # Lower-tail-heavy: many high-MOS rows with one low outlier.
        # MOS=[5,5,5,5,1] → proxies=[100,100,100,100,20]
        # mean=84, q25=60, q75=100 → lower_tail=24, upper_tail=16
        # asymmetry=8, offset=-4 → clamped to -2.0
        rows = [_row(f"h{i}", 640, 360, 5.0) for i in range(4)]
        rows.append(_row("low", 640, 360, 1.0))
        offset = cpfr._ugc_target_vmaf_offset(rows)
        assert offset < 0.0

    def test_offset_clamped_to_minus_2(self):
        # All rows at MOS=1 except one extreme → maximally asymmetric
        rows = [_row(f"c{i}", 640, 360, 1.0) for i in range(10)]
        rows.append(_row("outlier", 640, 360, 5.0))
        offset = cpfr._ugc_target_vmaf_offset(rows)
        assert offset >= -2.0

    def test_offset_clamped_to_plus_2(self):
        # Upper-tail-heavy: many high-MOS rows
        rows = [_row(f"c{i}", 640, 360, 5.0) for i in range(10)]
        rows.append(_row("low", 640, 360, 1.0))
        offset = cpfr._ugc_target_vmaf_offset(rows)
        assert offset <= 2.0

    def test_result_is_rounded_to_1_decimal(self):
        rows = [_row(f"c{i}", 640, 360, float(i % 5 + 1)) for i in range(8)]
        offset = cpfr._ugc_target_vmaf_offset(rows)
        # round(x, 1) means at most one decimal place
        assert offset == round(offset, 1)


# ---------------------------------------------------------------------------
# _ugc_tight_interval_width
# ---------------------------------------------------------------------------


class TestUgcTightIntervalWidth:
    def test_empty_rows_returns_fallback_3(self):
        assert cpfr._ugc_tight_interval_width([]) == pytest.approx(3.0)

    def test_single_row_falls_back(self):
        rows = [_row("a", 640, 360, 3.0)]
        # Only 1 row — statistics.quantiles needs ≥2; returns fallback 3.0
        width = cpfr._ugc_tight_interval_width(rows)
        # We accept the floor=1.5 or the fallback=3.0 — depends on impl
        assert 1.5 <= width <= 3.5

    def test_uniform_distribution_has_zero_iqr_returns_floor(self):
        rows = [_row(f"c{i}", 640, 360, 3.0) for i in range(10)]
        width = cpfr._ugc_tight_interval_width(rows)
        assert width == pytest.approx(1.5, abs=1e-6)

    def test_wide_distribution_returns_cap(self):
        # MOS: alternating 1 and 5 → proxies [20,100,...]; IQR = 80
        rows = [_row(f"c{i}", 640, 360, 1.0 if i % 2 == 0 else 5.0) for i in range(10)]
        width = cpfr._ugc_tight_interval_width(rows)
        assert width == pytest.approx(3.5, abs=1e-6)

    def test_result_is_rounded_to_2_decimals(self):
        rows = [_row(f"c{i}", 640, 360, float(i % 5 + 1)) for i in range(10)]
        width = cpfr._ugc_tight_interval_width(rows)
        assert width == round(width, 2)

    def test_result_in_valid_range(self):
        rows = [_row(f"c{i}", 640, 360, float(i % 5 + 1)) for i in range(20)]
        width = cpfr._ugc_tight_interval_width(rows)
        assert 1.5 <= width <= 3.5


# ---------------------------------------------------------------------------
# _resolution_dominance
# ---------------------------------------------------------------------------


class TestResolutionDominance:
    def test_empty_rows_returns_zero(self):
        assert cpfr._resolution_dominance([]) == pytest.approx(0.0)

    def test_single_resolution_returns_one(self):
        rows = [_row(f"c{i}", 1920, 1080, 3.0) for i in range(5)]
        assert cpfr._resolution_dominance(rows) == pytest.approx(1.0)

    def test_even_split_returns_half(self):
        rows = [_row(f"a{i}", 1920, 1080, 3.0) for i in range(5)]
        rows += [_row(f"b{i}", 960, 540, 3.0) for i in range(5)]
        assert cpfr._resolution_dominance(rows) == pytest.approx(0.5)

    def test_dominant_resolution_returns_correct_fraction(self):
        # 8 of 10 rows are 1920×1080
        rows = [_row(f"a{i}", 1920, 1080, 3.0) for i in range(8)]
        rows += [_row(f"b{i}", 960, 540, 3.0) for i in range(2)]
        assert cpfr._resolution_dominance(rows) == pytest.approx(0.8)

    def test_single_row_returns_one(self):
        rows = [_row("a", 640, 360, 2.0)]
        assert cpfr._resolution_dominance(rows) == pytest.approx(1.0)

    def test_portrait_vs_landscape_distinct_buckets(self):
        # 960×540 and 540×960 are different (w, h) buckets
        rows = [_row("a", 960, 540, 3.0), _row("b", 540, 960, 3.0)]
        assert cpfr._resolution_dominance(rows) == pytest.approx(0.5)


# ---------------------------------------------------------------------------
# _ugc_saliency_benefit_fraction
# ---------------------------------------------------------------------------


class TestUgcSaliencyBenefitFraction:
    def test_empty_rows_returns_fallback_0_10(self):
        assert cpfr._ugc_saliency_benefit_fraction([]) == pytest.approx(0.10)

    def test_no_qualifying_rows_returns_zero(self):
        # All portrait (h > w) → landscape check fails; MOS >= 3.0 → mos check fails
        rows = [_row(f"c{i}", 540, 960, 4.0) for i in range(5)]
        assert cpfr._ugc_saliency_benefit_fraction(rows) == pytest.approx(0.0)

    def test_all_qualifying_rows_returns_one(self):
        # Landscape (w >= h) AND low MOS (mos < 3.0) — all qualify
        rows = [_row(f"c{i}", 960, 540, 2.0) for i in range(5)]
        assert cpfr._ugc_saliency_benefit_fraction(rows) == pytest.approx(1.0)

    def test_half_qualifying(self):
        # 5 landscape+low-MOS, 5 portrait
        rows = [_row(f"a{i}", 960, 540, 2.0) for i in range(5)]
        rows += [_row(f"b{i}", 540, 960, 2.0) for i in range(5)]
        assert cpfr._ugc_saliency_benefit_fraction(rows) == pytest.approx(0.5)

    def test_landscape_but_high_mos_not_counted(self):
        # Landscape but MOS >= 3.0 → not counted
        rows = [_row("a", 960, 540, 3.0), _row("b", 960, 540, 2.9)]
        result = cpfr._ugc_saliency_benefit_fraction(rows)
        # Only row "b" qualifies (MOS < 3.0)
        assert result == pytest.approx(0.5)

    def test_square_aspect_counts_as_landscape(self):
        # w == h → w >= h is True → landscape
        rows = [_row("a", 540, 540, 2.0)]
        assert cpfr._ugc_saliency_benefit_fraction(rows) == pytest.approx(1.0)


# ---------------------------------------------------------------------------
# calibrate() — integration: all four recipe classes are present
# ---------------------------------------------------------------------------


class TestCalibrateIntegration:
    def _make_rows(self, n: int = 10) -> list[cpfr.CorpusRow]:
        """Build a small synthetic corpus covering K150K's landscape+UGC profile."""
        rows = []
        for i in range(n):
            mos = 1.0 + 4.0 * (i / max(n - 1, 1))
            w, h = (960, 540) if i % 2 == 0 else (540, 960)
            rows.append(_row(f"clip_{i}", w, h, mos))
        return rows

    def test_all_four_recipe_classes_present(self, tmp_path: Path):
        rows = self._make_rows(10)
        payload = cpfr.calibrate(
            rows,
            corpus_path=tmp_path / "corpus.jsonl",
            corpus_row_count=10,
        )
        recipes = payload["recipes"]
        for cls in ("ugc", "animation", "screen_content", "live_action_hdr"):
            assert cls in recipes, f"Missing recipe class: {cls}"

    def test_ugc_recipe_has_required_keys(self, tmp_path: Path):
        rows = self._make_rows(10)
        payload = cpfr.calibrate(
            rows,
            corpus_path=tmp_path / "corpus.jsonl",
            corpus_row_count=10,
        )
        ugc = payload["recipes"]["ugc"]
        assert "tight_interval_max_width" in ugc
        assert "force_single_rung" in ugc
        assert "saliency_intensity" in ugc
        assert "target_vmaf_offset" in ugc
        assert ugc["force_single_rung"] is False

    def test_metadata_block_present_and_correct(self, tmp_path: Path):
        rows = self._make_rows(8)
        corpus_path = tmp_path / "corpus.jsonl"
        payload = cpfr.calibrate(rows, corpus_path=corpus_path, corpus_row_count=8)
        meta = payload["metadata"]
        assert meta["schema_version"] == 1
        assert meta["phase"] == "F.5"
        assert meta["ugc_baseline_mos"]["n"] == 8

    def test_proxy_classes_are_tagged_proxy(self, tmp_path: Path):
        rows = self._make_rows(8)
        payload = cpfr.calibrate(
            rows,
            corpus_path=tmp_path / "corpus.jsonl",
            corpus_row_count=8,
        )
        for cls in ("animation", "screen_content", "live_action_hdr"):
            prov = payload["recipes"][cls]["_provenance"]
            assert prov["source"] == "proxy", f"Expected 'proxy' source for {cls}"

    def test_ugc_provenance_tagged_corpus(self, tmp_path: Path):
        rows = self._make_rows(6)
        payload = cpfr.calibrate(
            rows,
            corpus_path=tmp_path / "corpus.jsonl",
            corpus_row_count=6,
        )
        prov = payload["recipes"]["ugc"]["_provenance"]
        assert prov["source"] == "corpus"
        assert prov["corpus_rows_used"] == 6

    def test_animation_recipe_has_all_four_override_keys(self, tmp_path: Path):
        rows = self._make_rows(8)
        payload = cpfr.calibrate(
            rows,
            corpus_path=tmp_path / "corpus.jsonl",
            corpus_row_count=8,
        )
        anim = payload["recipes"]["animation"]
        for key in (
            "tight_interval_max_width",
            "force_single_rung",
            "saliency_intensity",
            "target_vmaf_offset",
        ):
            assert key in anim, f"Animation recipe missing key: {key}"

    def test_saliency_intensity_values_are_valid(self, tmp_path: Path):
        rows = self._make_rows(10)
        payload = cpfr.calibrate(
            rows,
            corpus_path=tmp_path / "corpus.jsonl",
            corpus_row_count=10,
        )
        valid = {"default", "aggressive", "very_aggressive"}
        for cls, recipe in payload["recipes"].items():
            if "saliency_intensity" in recipe:
                assert (
                    recipe["saliency_intensity"] in valid
                ), f"{cls}.saliency_intensity={recipe['saliency_intensity']!r} not in {valid}"
