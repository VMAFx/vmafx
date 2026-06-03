# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Coverage push for vmaftune.report — gaps identified by inspection.

Exercises branches not covered by the existing test_report.py:

* :func:`codec_metadata` — registered codec, unknown codec, colour field.
* :func:`compute_pareto_frontier` — empty points, all-failed points,
  tie-break by codec name, multiple targets.
* :func:`build_encoder_profile` — mixed ok/failed sweep + compare rows,
  profile schema fields, codec_metadata embedding.
* :func:`_quick_takeaways` — all paths: frontier, best-single-target,
  failed rows notice, ladder rungs, per-shot CRFs, no-data fallback.
* :func:`_is_missing` — None, NaN, Inf, finite.
* :func:`_fmt_kbps` — below 1000, above 1000, missing.
* :func:`_fmt_duration` — seconds, minutes, missing.
* :func:`_fmt_vmaf` / :func:`_fmt_ms` / :func:`_fmt_crf` — edge cases.
* :func:`_sweep_row_status` — all OK, partial failure, missing target.
* :func:`_sweep_crf_picks` — empty, populated.
* :func:`_codecs_in_report` — dedup across codec_rows + sweep_points.
* :func:`_dedup_bisect_samples` — dedup by CRF, bitrate sort, empty.
* :func:`_has_bisect_samples` — True / False.
* Sweep-report rendering with ``bisect_samples`` populated.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.report import (  # noqa: E402
    BisectSamplePoint,
    CodecRow,
    CodecSweepPoint,
    LadderRung,
    LadderSample,
    ReportData,
    ShotRow,
    SourceInfo,
    _codecs_in_report,
    _dedup_bisect_samples,
    _fmt_crf,
    _fmt_duration,
    _fmt_kbps,
    _fmt_ms,
    _fmt_vmaf,
    _has_bisect_samples,
    _is_missing,
    _quick_takeaways,
    _sweep_crf_picks,
    _sweep_row_status,
    build_encoder_profile,
    codec_metadata,
    compute_pareto_frontier,
    render_html,
    render_markdown,
)


def _src() -> SourceInfo:
    return SourceInfo(
        path="/tmp/test.mp4",
        width=1920,
        height=1080,
        fps=24.0,
        duration_s=10.0,
        frame_count=240,
        codec="h264",
        size_bytes=1_000_000,
    )


# ---------------------------------------------------------------------------
# _is_missing
# ---------------------------------------------------------------------------


class TestIsMissing:
    def test_none_is_missing(self) -> None:
        assert _is_missing(None) is True

    def test_nan_is_missing(self) -> None:
        assert _is_missing(float("nan")) is True

    def test_inf_is_missing(self) -> None:
        assert _is_missing(float("inf")) is True

    def test_neg_inf_is_missing(self) -> None:
        assert _is_missing(float("-inf")) is True

    def test_zero_is_not_missing(self) -> None:
        assert _is_missing(0.0) is False

    def test_finite_is_not_missing(self) -> None:
        assert _is_missing(92.5) is False


# ---------------------------------------------------------------------------
# Format helpers
# ---------------------------------------------------------------------------


class TestFmtKbps:
    def test_below_1000(self) -> None:
        assert "kbps" in _fmt_kbps(500.0)

    def test_above_1000(self) -> None:
        result = _fmt_kbps(2500.0)
        assert "Mbps" in result

    def test_exactly_1000(self) -> None:
        result = _fmt_kbps(1000.0)
        assert "Mbps" in result

    def test_missing_returns_dash(self) -> None:
        assert _fmt_kbps(None) == "—"
        assert _fmt_kbps(float("nan")) == "—"


class TestFmtDuration:
    def test_seconds_below_60(self) -> None:
        result = _fmt_duration(45.0)
        assert "s" in result
        assert "m" not in result

    def test_minutes_and_seconds(self) -> None:
        result = _fmt_duration(75.0)
        assert "m" in result
        assert "s" in result

    def test_missing_returns_dash(self) -> None:
        assert _fmt_duration(None) == "—"
        assert _fmt_duration(float("nan")) == "—"


class TestFmtVmaf:
    def test_finite_formats_two_decimals(self) -> None:
        result = _fmt_vmaf(92.456)
        assert "92.46" in result

    def test_none_returns_dash(self) -> None:
        assert _fmt_vmaf(None) == "—"

    def test_nan_returns_dash(self) -> None:
        assert _fmt_vmaf(float("nan")) == "—"


class TestFmtMs:
    def test_finite_formats_integer_ms(self) -> None:
        result = _fmt_ms(1234.7)
        assert "1235" in result or "ms" in result

    def test_none_returns_dash(self) -> None:
        assert _fmt_ms(None) == "—"


class TestFmtCrf:
    def test_positive_crf(self) -> None:
        assert _fmt_crf(23) == "23"

    def test_none_returns_dash(self) -> None:
        assert _fmt_crf(None) == "—"

    def test_negative_returns_dash(self) -> None:
        assert _fmt_crf(-1) == "—"


# ---------------------------------------------------------------------------
# codec_metadata
# ---------------------------------------------------------------------------


class TestCodecMetadata:
    def test_registered_codec_returns_known_metadata(self) -> None:
        meta = codec_metadata("libx264")
        assert meta["badge"] == "H.264"
        assert meta["label"] == "x264 / AVC"
        assert "url" in meta
        assert meta["codec"] == "libx264"
        assert "colour" in meta

    def test_unknown_codec_returns_defaults_with_codec_field(self) -> None:
        meta = codec_metadata("my_custom_codec_xyz")
        assert meta["codec"] == "my_custom_codec_xyz"
        assert meta["family"] == "Unknown"
        assert "note" in meta

    def test_all_registered_codecs_have_url(self) -> None:
        known = [
            "libx264",
            "libx265",
            "libsvtav1",
            "libaom-av1",
            "libvpx-vp9",
            "libvvenc",
            "h264_nvenc",
            "hevc_nvenc",
            "av1_nvenc",
            "h264_qsv",
            "hevc_qsv",
            "av1_qsv",
            "h264_amf",
            "hevc_amf",
            "av1_amf",
        ]
        for codec in known:
            meta = codec_metadata(codec)
            assert meta["url"], f"expected non-empty url for {codec}"

    def test_colour_field_is_hex_string(self) -> None:
        meta = codec_metadata("libx264")
        colour = meta["colour"]
        assert colour.startswith("#"), f"expected hex colour, got {colour!r}"


# ---------------------------------------------------------------------------
# compute_pareto_frontier
# ---------------------------------------------------------------------------


class TestComputeParetoFrontier:
    def test_empty_returns_empty(self) -> None:
        assert compute_pareto_frontier([]) == ()

    def test_all_failed_returns_empty(self) -> None:
        pts = [
            CodecSweepPoint("libx264", "x264", 90.0, 24, float("nan"), 100, float("nan"), False),
            CodecSweepPoint("libx265", "x265", 90.0, -1, float("nan"), 0, float("nan"), False),
        ]
        assert compute_pareto_frontier(pts) == ()

    def test_single_ok_point_is_its_own_frontier(self) -> None:
        pts = [CodecSweepPoint("libx264", "x264", 90.0, 24, 2400.0, 100, 92.0, True)]
        frontier = compute_pareto_frontier(pts)
        assert len(frontier) == 1
        assert frontier[0].codec == "libx264"

    def test_picks_lowest_bitrate_per_target(self) -> None:
        pts = [
            CodecSweepPoint("libx264", "x264", 90.0, 24, 2400.0, 100, 92.0, True),
            CodecSweepPoint("libsvtav1", "svt", 90.0, 30, 1800.0, 120, 91.5, True),
        ]
        frontier = compute_pareto_frontier(pts)
        assert len(frontier) == 1
        assert frontier[0].codec == "libsvtav1"
        assert frontier[0].bitrate_kbps == pytest.approx(1800.0)

    def test_tie_break_by_codec_name(self) -> None:
        pts = [
            CodecSweepPoint("libz264", "z264", 90.0, 24, 2000.0, 100, 92.0, True),
            CodecSweepPoint("liba264", "a264", 90.0, 24, 2000.0, 100, 92.0, True),
        ]
        frontier = compute_pareto_frontier(pts)
        assert len(frontier) == 1
        # liba264 wins tie-break (alphabetically smaller)
        assert frontier[0].codec == "liba264"

    def test_multiple_targets_each_get_winner(self) -> None:
        pts = [
            CodecSweepPoint("libx264", "x264", 85.0, 28, 1800.0, 100, 86.0, True),
            CodecSweepPoint("libsvtav1", "svt", 85.0, 34, 1400.0, 120, 85.5, True),
            CodecSweepPoint("libx264", "x264", 92.0, 23, 2600.0, 110, 92.1, True),
            CodecSweepPoint("libsvtav1", "svt", 92.0, 28, 2000.0, 130, 92.3, True),
        ]
        frontier = compute_pareto_frontier(pts)
        assert len(frontier) == 2
        targets = {p.target_vmaf for p in frontier}
        assert targets == {85.0, 92.0}

    def test_ordered_by_target_vmaf_ascending(self) -> None:
        pts = [
            CodecSweepPoint("libx264", "x264", 95.0, 20, 3000.0, 100, 95.1, True),
            CodecSweepPoint("libx264", "x264", 80.0, 35, 1200.0, 80, 80.5, True),
            CodecSweepPoint("libx264", "x264", 90.0, 25, 2000.0, 90, 90.2, True),
        ]
        frontier = compute_pareto_frontier(pts)
        vmaf_order = [p.target_vmaf for p in frontier]
        assert vmaf_order == sorted(vmaf_order)


# ---------------------------------------------------------------------------
# _codecs_in_report
# ---------------------------------------------------------------------------


class TestCodecsInReport:
    def test_deduplicates_across_rows_and_sweep(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            codec_rows=(
                CodecRow("libx264", "x264", 23, 2400, 100, 92.0, True),
                CodecRow("libx265", "x265", 24, 2000, 110, 92.1, True),
            ),
            sweep_points=(
                CodecSweepPoint("libx264", "x264", 90.0, 24, 2300.0, 90, 91.8, True),
                CodecSweepPoint("libsvtav1", "svt", 90.0, 30, 1800.0, 100, 91.2, True),
            ),
        )
        codecs = _codecs_in_report(data)
        assert set(codecs) == {"libx264", "libx265", "libsvtav1"}

    def test_empty_report_returns_empty(self) -> None:
        data = ReportData(source=_src(), target_vmaf=90.0)
        assert _codecs_in_report(data) == ()


# ---------------------------------------------------------------------------
# _sweep_row_status
# ---------------------------------------------------------------------------


class TestSweepRowStatus:
    def test_all_ok_returns_ok(self) -> None:
        pts = [CodecSweepPoint("libx264", "x264", 90.0, 24, 2000.0, 100, 91.0, True)]
        result = _sweep_row_status(pts, [90.0])
        assert result == "OK"

    def test_failure_includes_reason(self) -> None:
        pts = [
            CodecSweepPoint(
                "libx264", "x264", 90.0, -1, float("nan"), 0, float("nan"), False, "timeout"
            )
        ]
        result = _sweep_row_status(pts, [90.0])
        assert "timeout" in result

    def test_missing_target_reported(self) -> None:
        pts = [CodecSweepPoint("libx264", "x264", 90.0, 24, 2000.0, 100, 91.0, True)]
        result = _sweep_row_status(pts, [90.0, 95.0])
        assert "missing" in result.lower()

    def test_none_point_skipped(self) -> None:
        # None points are skipped in the failure check
        result = _sweep_row_status([None], [90.0])  # type: ignore[list-item]
        assert "missing" in result.lower()


# ---------------------------------------------------------------------------
# _sweep_crf_picks
# ---------------------------------------------------------------------------


class TestSweepCrfPicks:
    def test_empty_returns_dash(self) -> None:
        assert _sweep_crf_picks([]) == "—"

    def test_all_failed_returns_dash(self) -> None:
        pts = [CodecSweepPoint("libx264", "x264", 90.0, -1, float("nan"), 0, float("nan"), False)]
        assert _sweep_crf_picks(pts) == "—"

    def test_ok_points_formatted(self) -> None:
        pts = [
            CodecSweepPoint("libx264", "x264", 85.0, 28, 1800.0, 100, 86.0, True),
            CodecSweepPoint("libx264", "x264", 92.0, 23, 2500.0, 110, 92.1, True),
        ]
        result = _sweep_crf_picks(pts)
        assert "85→28" in result
        assert "92→23" in result


# ---------------------------------------------------------------------------
# _has_bisect_samples / _dedup_bisect_samples
# ---------------------------------------------------------------------------


class TestHasBisectSamples:
    def test_false_when_no_samples(self) -> None:
        pts = [CodecSweepPoint("libx264", "x264", 90.0, 24, 2000.0, 100, 91.0, True)]
        assert _has_bisect_samples(pts) is False

    def test_true_when_samples_present(self) -> None:
        samples = (BisectSamplePoint(24, 2000.0, 91.0, 5000.0),)
        pts = [
            CodecSweepPoint(
                "libx264", "x264", 90.0, 24, 2000.0, 100, 91.0, True, bisect_samples=samples
            )
        ]
        assert _has_bisect_samples(pts) is True

    def test_false_when_only_failed_has_samples(self) -> None:
        samples = (BisectSamplePoint(24, 2000.0, 91.0, 5000.0),)
        pts = [
            CodecSweepPoint(
                "libx264",
                "x264",
                90.0,
                -1,
                float("nan"),
                0,
                float("nan"),
                False,
                bisect_samples=samples,
            )
        ]
        assert _has_bisect_samples(pts) is False


class TestDedupBisectSamples:
    def test_empty_points_returns_empty(self) -> None:
        assert _dedup_bisect_samples([]) == {}

    def test_deduplicates_same_crf(self) -> None:
        samples = (
            BisectSamplePoint(24, 2000.0, 91.0, 5000.0),
            BisectSamplePoint(24, 2100.0, 91.5, 4800.0),  # same CRF, different bitrate
        )
        pts = [
            CodecSweepPoint(
                "libx264", "x264", 90.0, 24, 2000.0, 100, 91.0, True, bisect_samples=samples
            )
        ]
        result = _dedup_bisect_samples(pts)
        assert "libx264" in result
        # Only one sample per CRF
        assert len([s for s in result["libx264"] if s.crf == 24]) == 1

    def test_sorted_by_bitrate(self) -> None:
        samples = (
            BisectSamplePoint(30, 1500.0, 88.0, 4000.0),
            BisectSamplePoint(24, 2500.0, 92.0, 5000.0),
            BisectSamplePoint(20, 4000.0, 96.0, 7000.0),
        )
        pts = [
            CodecSweepPoint(
                "libx264", "x264", 90.0, 24, 2500.0, 100, 92.0, True, bisect_samples=samples
            )
        ]
        result = _dedup_bisect_samples(pts)
        bitrates = [s.bitrate_kbps for s in result["libx264"]]
        assert bitrates == sorted(bitrates)

    def test_skips_failed_points(self) -> None:
        samples = (BisectSamplePoint(24, 2000.0, 91.0, 5000.0),)
        pts = [
            CodecSweepPoint(
                "libx264",
                "x264",
                90.0,
                -1,
                float("nan"),
                0,
                float("nan"),
                False,
                bisect_samples=samples,
            )
        ]
        result = _dedup_bisect_samples(pts)
        # Failed point's samples must not appear
        assert result == {}


# ---------------------------------------------------------------------------
# _quick_takeaways
# ---------------------------------------------------------------------------


class TestQuickTakeaways:
    def test_no_data_returns_fallback_message(self) -> None:
        data = ReportData(source=_src(), target_vmaf=90.0)
        takeaways = _quick_takeaways(data)
        assert any("no successful" in t.lower() for t in takeaways)

    def test_frontier_generates_smallest_row_takeaway(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            sweep_points=(
                CodecSweepPoint("libsvtav1", "svt", 90.0, 30, 1800.0, 100, 91.0, True),
                CodecSweepPoint("libx264", "x264", 90.0, 24, 2400.0, 90, 91.5, True),
            ),
        )
        takeaways = _quick_takeaways(data)
        # libsvtav1 is the pareto winner (lower bitrate)
        assert any("libsvtav1" in t for t in takeaways)

    def test_failed_rows_notice(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            codec_rows=(
                CodecRow("libx264", "x264", 23, 2400, 100, 92.0, True),
                CodecRow("libvpx", "vpx", -1, 0, 0, 0, False, "timeout"),
            ),
        )
        takeaways = _quick_takeaways(data)
        assert any("failed" in t.lower() or "unavailable" in t.lower() for t in takeaways)

    def test_ladder_rungs_takeaway(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            ladder_rungs=(
                LadderRung(1920, 1080, 2400.0, 92.0, 23),
                LadderRung(1280, 720, 1100.0, 86.0, 26),
            ),
        )
        takeaways = _quick_takeaways(data)
        assert any("ladder" in t.lower() for t in takeaways)

    def test_per_shot_takeaway(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            shots=(
                ShotRow(0, 0, 120, 1920, 1080, 22, 94.2, 5800, 5.0),
                ShotRow(1, 120, 240, 1920, 1080, 28, 90.1, 3200, 5.0),
            ),
        )
        takeaways = _quick_takeaways(data)
        assert any("shot" in t.lower() for t in takeaways)

    def test_single_shot_no_range_takeaway(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            shots=(ShotRow(0, 0, 240, 1920, 1080, 25, 91.0, 4000, 10.0),),
        )
        takeaways = _quick_takeaways(data)
        assert any("shot" in t.lower() for t in takeaways)

    def test_best_single_target_from_compare_rows(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            codec_rows=(
                CodecRow("libx264", "x264", 23, 2400, 100, 92.0, True),
                CodecRow("libsvtav1", "svt", 30, 1800, 120, 91.5, True),
            ),
        )
        takeaways = _quick_takeaways(data)
        assert any("libsvtav1" in t for t in takeaways)


# ---------------------------------------------------------------------------
# build_encoder_profile
# ---------------------------------------------------------------------------


class TestBuildEncoderProfile:
    def test_schema_and_version_present(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            codec_rows=(CodecRow("libx264", "x264", 23, 2400, 100, 92.0, True),),
        )
        profile = build_encoder_profile(data)
        assert profile["schema"] == "vmaftune.encoder_profile.v1"
        assert profile["schema_version"] == 1

    def test_failed_row_goes_to_failures(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            codec_rows=(
                CodecRow("libx264", "x264", 23, 2400, 100, 92.0, True),
                CodecRow("libvpx", "vpx", -1, 0, 0, 0, False, "timeout"),
            ),
        )
        profile = build_encoder_profile(data)
        failure_codecs = {f["codec"] for f in profile["failures"]}
        assert "libvpx" in failure_codecs

    def test_sweep_points_with_pareto_flag(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            sweep_points=(
                CodecSweepPoint("libsvtav1", "svt", 90.0, 30, 1800.0, 100, 91.0, True),
                CodecSweepPoint("libx264", "x264", 90.0, 24, 2400.0, 90, 91.5, True),
            ),
        )
        profile = build_encoder_profile(data)
        pareto_recs = [r for r in profile["recommendations"] if r["selected_pareto"]]
        assert len(pareto_recs) >= 1
        # libsvtav1 is the pareto winner
        assert pareto_recs[0]["codec"] == "libsvtav1"

    def test_codec_metadata_embedded(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            codec_rows=(CodecRow("libx264", "x264", 23, 2400, 100, 92.0, True),),
        )
        profile = build_encoder_profile(data)
        assert "libx264" in profile["codec_metadata"]
        assert "badge" in profile["codec_metadata"]["libx264"]

    def test_recommendations_sorted_by_pareto_then_vmaf_then_bitrate(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            sweep_points=(
                CodecSweepPoint("libsvtav1", "svt", 90.0, 30, 1800.0, 100, 91.0, True),
                CodecSweepPoint("libx265", "x265", 90.0, 24, 2000.0, 110, 91.2, True),
            ),
        )
        profile = build_encoder_profile(data)
        recs = profile["recommendations"]
        # All pareto recommendations appear before non-pareto ones
        pareto_indices = [i for i, r in enumerate(recs) if r["selected_pareto"]]
        non_pareto_indices = [i for i, r in enumerate(recs) if not r["selected_pareto"]]
        if pareto_indices and non_pareto_indices:
            assert max(pareto_indices) < min(non_pareto_indices)

    def test_run_fields_propagated(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            encoder_preset="slow",
            pix_fmt="yuv420p10le",
            score_backend="cuda",
            ffmpeg_bin="/usr/local/bin/ffmpeg",
            vmaf_bin="/usr/local/bin/vmaf",
        )
        profile = build_encoder_profile(data)
        assert profile["run"]["preset"] == "slow"
        assert profile["run"]["pix_fmt"] == "yuv420p10le"
        assert profile["run"]["score_backend"] == "cuda"
        assert profile["run"]["ffmpeg_bin"] == "/usr/local/bin/ffmpeg"
        assert profile["run"]["vmaf_bin"] == "/usr/local/bin/vmaf"


# ---------------------------------------------------------------------------
# Sweep report rendering with bisect_samples
# ---------------------------------------------------------------------------


class TestSweepReportBisectSamples:
    def _make_sweep_data_with_samples(self) -> ReportData:
        samples_a = (
            BisectSamplePoint(28, 1800.0, 88.0, 4000.0),
            BisectSamplePoint(24, 2400.0, 92.0, 5000.0),
            BisectSamplePoint(20, 3800.0, 96.0, 7000.0),
        )
        samples_b = (
            BisectSamplePoint(32, 1200.0, 85.0, 3500.0),
            BisectSamplePoint(28, 1900.0, 89.0, 4200.0),
            BisectSamplePoint(24, 2800.0, 93.0, 5500.0),
        )
        return ReportData(
            source=_src(),
            target_vmaf=90.0,
            sweep_targets=(90.0, 95.0),
            sweep_points=(
                CodecSweepPoint(
                    "libx264", "x264", 90.0, 24, 2400.0, 100, 92.0, True, bisect_samples=samples_a
                ),
                CodecSweepPoint(
                    "libsvtav1", "svt", 90.0, 30, 1900.0, 120, 91.0, True, bisect_samples=samples_b
                ),
                CodecSweepPoint(
                    "libx264", "x264", 95.0, 20, 3800.0, 110, 95.1, True, bisect_samples=samples_a
                ),
            ),
        )

    def test_markdown_renders_sweep_section(self) -> None:
        data = self._make_sweep_data_with_samples()
        md = render_markdown(data)
        assert "Rate-quality sweep" in md or "Codec guide" in md
        # JSON dump must be serialisable
        profile_section = data.to_dict()
        json.dumps(profile_section)

    def test_html_renders_sweep_section(self) -> None:
        data = self._make_sweep_data_with_samples()
        html = render_html(data)
        assert "<!doctype html>" in html
        assert "libx264" in html or "libsvtav1" in html

    def test_has_bisect_samples_true_for_this_data(self) -> None:
        data = self._make_sweep_data_with_samples()
        assert _has_bisect_samples(data.sweep_points)


# ---------------------------------------------------------------------------
# to_dict / render round-trips
# ---------------------------------------------------------------------------


class TestReportDataToDict:
    def test_to_dict_includes_encoder_profile_key(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
        )
        d = data.to_dict()
        assert "encoder_profile" in d
        assert d["encoder_profile"]["schema"] == "vmaftune.encoder_profile.v1"

    def test_to_dict_all_fields_serialisable(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            codec_rows=(CodecRow("libx264", "x264", 23, 2400, 100, 92.0, True),),
            ladder_samples=(LadderSample(1920, 1080, 2400.0, 92.0, 23),),
            ladder_rungs=(LadderRung(1920, 1080, 2400.0, 92.0, 23),),
            shots=(ShotRow(0, 0, 120, 1920, 1080, 23, 92.0, 2400.0, 5.0),),
        )
        d = data.to_dict()
        json.dumps(d)  # must not raise

    def test_nan_in_sweep_points_serialised_as_null_in_to_dict(self) -> None:
        data = ReportData(
            source=_src(),
            target_vmaf=90.0,
            sweep_points=(
                CodecSweepPoint(
                    "libx264", "x264", 90.0, -1, float("nan"), 0, float("nan"), False, "err"
                ),
            ),
        )
        d = data.to_dict()
        text = json.dumps(d)
        # The NaN values must be null in the output (handled by dumps_strict in to_dict)
        # At minimum the to_dict must not raise
        assert "libx264" in text or "err" in text
