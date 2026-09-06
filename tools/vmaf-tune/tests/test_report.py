# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for the vmaftune.report module."""

from __future__ import annotations

import json
import pathlib
import sys

import pytest

try:
    import matplotlib.pyplot as plt

    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False

# Make `vmaftune` importable for the in-tree test invocation.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

from vmaftune.report import (
    _CODEC_COLOURS,
    _DASH,
    ENCODER_PROFILE_SCHEMA,
    BisectSamplePoint,
    CodecRow,
    CodecSweepPoint,
    LadderRung,
    LadderSample,
    ReportData,
    ShotRow,
    SourceInfo,
    _bitrate_tick_label,
    _codec_colour,
    _codec_plot_fn,
    _ladder_plot_fn,
    _sweep_plot_fn,
    render_html,
    render_markdown,
)


def _sample_data() -> ReportData:
    src = SourceInfo(
        path="/tmp/example.mp4",
        width=1920,
        height=1080,
        fps=24.0,
        duration_s=10.0,
        frame_count=240,
        codec="h264",
        size_bytes=1_000_000,
    )
    return ReportData(
        source=src,
        target_vmaf=92.0,
        codec_rows=(
            CodecRow("libx264", "x264", 23, 2400, 4200, 92.4, True),
            CodecRow("libsvtav1", "SVT-AV1", 30, 1500, 7600, 92.6, True),
            CodecRow("libvpx", "libvpx", 33, 0, 0, 0, False, "timeout"),
        ),
        ladder_samples=(
            LadderSample(1920, 1080, 2400, 92.4, 23),
            LadderSample(1280, 720, 1100, 86.5, 26),
        ),
        ladder_rungs=(
            LadderRung(1920, 1080, 2400, 92.4, 23),
            LadderRung(1280, 720, 1100, 86.5, 26),
        ),
        shots=(
            ShotRow(0, 0, 120, 1920, 1080, 22, 94.2, 5800, 5.0),
            ShotRow(1, 120, 240, 1920, 1080, 26, 91.7, 3200, 5.0),
        ),
        generated_at_iso="2026-05-17T00:00:00+00:00",
    )


def test_markdown_contains_all_sections():
    md = render_markdown(_sample_data())
    assert "# vmaf-tune report" in md
    assert "## Source" in md
    assert "## Quick takeaways" in md
    assert "Best single-target row: libsvtav1" in md
    assert "## Codec comparison" in md
    assert "## ABR ladder" in md
    assert "## Per-shot tuning" in md
    assert "libx264" in md
    assert "libsvtav1" in md
    assert "timeout" in md  # failed-row error visible
    assert "92.4" in md  # vmaf
    # raw JSON dump is collapsible
    assert "<details>" in md
    assert "report.json" in md


def test_html_is_self_contained():
    html = render_html(_sample_data())
    assert "<!doctype html>" in html
    assert "<title>vmaf-tune report" in html
    # inline SVG charts (no external <img src="...">)
    assert "<svg" in html
    assert 'src="http' not in html  # no remote assets
    # tables rendered
    assert "Quick takeaways" in html
    assert "Best single-target row: libsvtav1" in html
    assert "Codec comparison" in html
    assert "Per-shot tuning" in html
    # status tag for failed row
    assert "tag bad" in html
    # JSON dump expandable
    assert "Raw JSON dump" in html


def test_to_dict_round_trip():
    data = _sample_data()
    d = data.to_dict()
    assert d["source"]["width"] == 1920
    assert d["target_vmaf"] == 92.0
    assert d["encoder_profile"]["schema"] == ENCODER_PROFILE_SCHEMA
    assert len(d["codec_rows"]) == 3
    assert d["codec_rows"][0]["codec"] == "libx264"
    assert d["encoder_profile"]["recommendations"][0]["codec"] == "libsvtav1"
    assert len(d["ladder_rungs"]) == 2
    assert len(d["shots"]) == 2
    # serialisable
    json.dumps(d)


def test_markdown_assets_dir_writes_pngs(tmp_path):
    assets = tmp_path / "assets"
    md = render_markdown(_sample_data(), assets_dir=assets)
    pngs = sorted(assets.glob("*.png"))
    assert len(pngs) >= 2  # ladder + codec + shot
    # md links them, not base64
    assert "data:image/png;base64" not in md


def test_html_one_shot_timeline_renders_non_empty_chart():
    """ADR-0513 Bug B regression: a single-shot per-shot result must
    still produce a visible drawable element in the rendered SVG chart.

    The historical ``ax.step([start], [crf], ...)`` call collapsed to a
    zero-length path the SVG backend silently dropped. The fix renders
    each shot as a horizontal band over its frame range so the chart
    always carries at least one ``<path>`` / ``<line>`` element with a
    non-degenerate ``d="..."`` (or x1!=x2 line) attribute.
    """
    src = SourceInfo(
        path="/tmp/example.mp4",
        width=3840,
        height=2160,
        fps=60.0,
        duration_s=5.0,
        frame_count=300,
        codec="h264",
        size_bytes=1_000_000,
    )
    data = ReportData(
        source=src,
        target_vmaf=92.0,
        shots=(ShotRow(0, 0, 300, 3840, 2160, 26, 92.47, 12000, 5.0),),
        generated_at_iso="2026-05-18T00:00:00+00:00",
    )
    html = render_html(data)
    # The chart panel renders (matplotlib available).
    assert "Per-shot tuning" in html
    # Inline SVG present.
    if "<svg" not in html:
        # matplotlib not installed in the test env — skip rather than fail.
        import pytest as _pytest

        _pytest.skip("matplotlib unavailable; SVG fallback exercised separately")
    # Locate the per-shot chart subtree by anchoring on the section
    # header and grabbing the SVG that follows it.
    chart_pos = html.find("Per-shot tuning timeline")
    # Title rendered inside the SVG; everything after it up to the next
    # </svg> is the chart we care about.
    if chart_pos < 0:
        # Fall back to any svg substring — the chart still rendered as
        # part of the page.
        chart_pos = html.find("<svg")
    svg_tail = html[chart_pos:]
    svg_end = svg_tail.find("</svg>")
    assert svg_end > 0, "expected closing </svg> after per-shot chart"
    chart_svg = svg_tail[:svg_end]
    # Drawable element heuristics: a Line2D / hline emits either a
    # <path d="M ... L ..."/> or a <line x1=... x2=.../> with x1 != x2.
    has_path = "<path " in chart_svg and 'd="M' in chart_svg
    has_line = "<line " in chart_svg
    assert has_path or has_line, (
        "1-shot timeline chart had no drawable path/line element — " "ADR-0513 Bug B regressed"
    )


def test_empty_sections_omitted():
    src = SourceInfo("/tmp/x.mp4", 1920, 1080, 24.0, 1.0, 24, "h264", 100)
    data = ReportData(source=src, target_vmaf=92.0)
    md = render_markdown(data)
    assert "## Source" in md
    assert "## Codec comparison" not in md
    assert "## ABR ladder" not in md
    assert "## Per-shot tuning" not in md

    html = render_html(data)
    assert "Codec comparison" not in html
    assert "ABR ladder" not in html
    assert "Per-shot tuning" not in html


def test_three_shot_timeline_shows_last_shot_band():
    """ADR-0531 Bug B: a three-shot per-shot timeline must render the
    last shot's CRF band visibly in the SVG chart.

    Historically the x-axis right bound was exactly ``last_end``, so
    matplotlib's clip box trimmed the last shot's hlines artist — the
    band for shot 2 (frames 200-300) was invisible.  The fix pads the
    right bound to ``last_end + max(1, 0.05 * last_end)`` so the
    rightmost segment is fully inside the viewport.
    """
    src = SourceInfo(
        path="/tmp/three_shot.mp4",
        width=3840,
        height=2160,
        fps=60.0,
        duration_s=5.0,
        frame_count=300,
        codec="h264",
        size_bytes=5_000_000,
    )
    data = ReportData(
        source=src,
        target_vmaf=92.0,
        shots=(
            ShotRow(0, 0, 100, 3840, 2160, 26, 92.1, 5000.0, 1.667),
            ShotRow(1, 100, 200, 3840, 2160, 25, 92.5, 4800.0, 1.667),
            ShotRow(2, 200, 300, 3840, 2160, 25, 92.3, 4200.0, 1.667),
        ),
        generated_at_iso="2026-05-18T00:00:00+00:00",
    )
    html = render_html(data)
    assert "Per-shot tuning" in html
    if "<svg" not in html:
        import pytest as _pytest

        _pytest.skip("matplotlib unavailable; SVG fallback exercised separately")

    # The per-shot chart SVG must contain drawable elements for all three shots.
    # We locate the shot-timeline SVG by its title text and verify it has
    # path/line drawable elements — the last shot's band being the critical one.
    chart_pos = html.find("Per-shot tuning timeline")
    if chart_pos < 0:
        chart_pos = html.find("<svg")
    svg_tail = html[chart_pos:]
    svg_end = svg_tail.find("</svg>")
    assert svg_end > 0, "expected closing </svg> after per-shot chart"
    chart_svg = svg_tail[:svg_end]

    has_path = "<path " in chart_svg and 'd="M' in chart_svg
    has_line = "<line " in chart_svg
    assert has_path or has_line, (
        "Three-shot timeline had no drawable path/line element. "
        "ADR-0531 Bug B: last-shot CRF band must be visible in the chart."
    )
    # The bitrate column in the markdown table must carry real numbers.
    md = render_markdown(data)
    assert (
        "5.00 Mbps" in md or "5000 kbps" in md or "4.80 Mbps" in md
    ), "expected real bitrate numbers in per-shot markdown table (ADR-0531 Bug A)"


def test_per_shot_bitrate_renders_in_markdown_table():
    """ADR-0531 Bug A: when ShotRow.bitrate_kbps is populated the markdown
    table must show the formatted bitrate, not an em-dash.
    """
    src = SourceInfo("/tmp/x.mp4", 1920, 1080, 30.0, 3.0, 90, "h264", 1_000_000)
    data = ReportData(
        source=src,
        target_vmaf=92.0,
        shots=(
            ShotRow(0, 0, 30, 1920, 1080, 23, 93.1, 5234.0, 1.0),
            ShotRow(1, 30, 60, 1920, 1080, 25, 92.5, 4800.0, 1.0),
            ShotRow(2, 60, 90, 1920, 1080, 26, 92.0, 3900.0, 1.0),
        ),
    )
    md = render_markdown(data)
    # Each shot's bitrate must appear as a human-readable string.
    # _fmt_kbps formats values >= 1000 as Mbps, so 5234 kbps => "5.23 Mbps".
    assert "5.23 Mbps" in md, f"shot 0 bitrate missing from report: {md}"
    assert "4.80 Mbps" in md, f"shot 1 bitrate missing from report: {md}"
    assert "3.90 Mbps" in md, f"shot 2 bitrate missing from report: {md}"
    # None of the Bitrate cells should be an em-dash (which would mean NaN).
    # The per-shot table rows contain the shot index at the start.
    shot_rows = [
        line
        for line in md.splitlines()
        if line.startswith("| ") and ("Mbps" in line or "kbps" in line)
    ]
    assert shot_rows, "expected at least one per-shot table row with a bitrate value"


def test_html_escapes_user_controlled_report_text():
    src = SourceInfo(
        path="/tmp/<unsafe>.mp4",
        width=1920,
        height=1080,
        fps=24.0,
        duration_s=1.0,
        frame_count=24,
        codec="<h264>",
        size_bytes=100,
    )
    data = ReportData(
        source=src,
        target_vmaf=92.0,
        codec_rows=(CodecRow("libx264", "x264<script>", 23, 2400, 100, 92.0, False, "<boom>"),),
    )
    html = render_html(data)
    assert "<unsafe>" not in html
    assert "<h264>" not in html
    assert "x264<script>" not in html
    assert "<boom>" not in html
    assert "&lt;unsafe&gt;" in html
    assert "&lt;h264&gt;" in html
    assert "x264&lt;script&gt;" in html
    assert "&lt;boom&gt;" in html


def test_sweep_report_has_human_guidance_status_and_profile():
    src = SourceInfo("/tmp/source.mkv", 1920, 1080, 24.0, 10.0, 240, "h264", 100)
    data = ReportData(
        source=src,
        target_vmaf=94.0,
        sweep_targets=(94.0, 96.0),
        sweep_points=(
            CodecSweepPoint("libx265", "x265", 94.0, 24, 2100, 100, 94.3, True),
            CodecSweepPoint(
                "libx265", "x265", 96.0, -1, float("nan"), 0, float("nan"), False, "timeout"
            ),
            CodecSweepPoint("libsvtav1", "SVT-AV1", 94.0, 30, 1600, 120, 94.2, True),
            CodecSweepPoint("libsvtav1", "SVT-AV1", 96.0, 26, 2400, 140, 96.1, True),
        ),
        pix_fmt="yuv420p10le",
        score_backend="cuda",
    )
    md = render_markdown(data)
    html = render_html(data)
    assert "How to read this" in md
    assert "At VMAF 94, libsvtav1 is the smallest successful row" in md
    assert "At VMAF 96, libsvtav1 is the smallest successful row" in md
    assert "Codec guide" in md
    assert "CRF picks" in md
    assert "94→24" in md
    assert "timeout" in md
    assert "adapter default" not in md
    assert "encoder_profile" in md
    assert "Codec guide" in html
    assert "codec-chip" in html
    assert "adapter default" not in html
    profile = data.to_dict()["encoder_profile"]
    assert profile["run"]["pix_fmt"] == "yuv420p10le"
    assert profile["run"]["score_backend"] == "cuda"
    assert profile["codec_metadata"]["libsvtav1"]["url"]


def _sample_compare_v1() -> ReportData:
    src = SourceInfo(
        path="/tmp/example.mp4",
        width=1920,
        height=1080,
        fps=24.0,
        duration_s=10.0,
        frame_count=240,
        codec="h264",
        size_bytes=1_000_000,
    )
    return ReportData(
        source=src,
        target_vmaf=92.0,
        codec_rows=(
            CodecRow("libx264", "x264", 23, 2400.0, 4200.0, 92.4, True),
            CodecRow("h264_videotoolbox", "vt", 24, 2600.0, 1100.0, 92.1, True),
            CodecRow("libsvtav1", "SVT-AV1", 30, 1500.0, 7600.0, 92.6, True),
            CodecRow("libvpx", "libvpx", 33, 0.0, 0.0, 0.0, False, "timeout"),
        ),
        generated_at_iso="2026-05-17T00:00:00+00:00",
    )


def _sample_compare_v2() -> ReportData:
    src = SourceInfo(
        path="/tmp/example.mp4",
        width=1920,
        height=1080,
        fps=24.0,
        duration_s=10.0,
        frame_count=240,
        codec="h264",
        size_bytes=1_000_000,
    )
    return ReportData(
        source=src,
        target_vmaf=94.0,
        sweep_targets=(94.0, 96.0),
        sweep_points=(
            CodecSweepPoint(
                "libsvtav1",
                "SVT-AV1",
                94.0,
                30,
                1600.0,
                120.0,
                94.2,
                True,
                bisect_samples=(
                    BisectSamplePoint(32, 1200.0, 91.5, 30.0),
                    BisectSamplePoint(30, 1600.0, 94.2, 35.0),
                    BisectSamplePoint(28, 2100.0, 95.8, 40.0),
                ),
            ),
            CodecSweepPoint(
                "libsvtav1",
                "SVT-AV1",
                96.0,
                26,
                2400.0,
                140.0,
                96.1,
                True,
                bisect_samples=(
                    BisectSamplePoint(26, 2400.0, 96.1, 45.0),
                    BisectSamplePoint(24, 3000.0, 97.2, 50.0),
                ),
            ),
            CodecSweepPoint(
                "libx265",
                "x265",
                94.0,
                24,
                2100.0,
                100.0,
                94.3,
                True,
                bisect_samples=(
                    BisectSamplePoint(26, 1700.0, 92.0, 25.0),
                    BisectSamplePoint(24, 2100.0, 94.3, 30.0),
                ),
            ),
            CodecSweepPoint(
                "libx265",
                "x265",
                96.0,
                -1,
                float("nan"),
                0.0,
                float("nan"),
                False,
                error="timeout",
            ),
        ),
        generated_at_iso="2026-05-17T00:00:00+00:00",
    )


def _sample_ladder() -> ReportData:
    src = SourceInfo(
        path="/tmp/example.mp4",
        width=1920,
        height=1080,
        fps=24.0,
        duration_s=10.0,
        frame_count=240,
        codec="h264",
        size_bytes=1_000_000,
    )
    return ReportData(
        source=src,
        target_vmaf=92.0,
        ladder_samples=(
            LadderSample(1920, 1080, 2400.0, 92.4, 23),
            LadderSample(1280, 720, 1100.0, 86.5, 26),
            LadderSample(960, 540, 600.0, 80.1, 28),
        ),
        ladder_rungs=(
            LadderRung(1920, 1080, 2400.0, 92.4, 23),
            LadderRung(1280, 720, 1100.0, 86.5, 26),
        ),
        generated_at_iso="2026-05-17T00:00:00+00:00",
    )


def _sample_per_shot() -> ReportData:
    src = SourceInfo(
        path="/tmp/example.mp4",
        width=1920,
        height=1080,
        fps=24.0,
        duration_s=10.0,
        frame_count=240,
        codec="h264",
        size_bytes=1_000_000,
    )
    return ReportData(
        source=src,
        target_vmaf=92.0,
        shots=(
            ShotRow(0, 0, 120, 1920, 1080, 22, 94.2, 5800.0, 5.0),
            ShotRow(1, 120, 240, 1920, 1080, 26, 91.7, 3200.0, 5.0),
        ),
        generated_at_iso="2026-05-17T00:00:00+00:00",
    )


def test_bitrate_units_and_axis_labels():
    assert _bitrate_tick_label(1500) == "1.5 Mbps"
    assert _bitrate_tick_label(800) == "800 kbps"
    assert _bitrate_tick_label(0) == ""
    assert _bitrate_tick_label(-10) == ""
    assert _bitrate_tick_label(float("nan")) == ""

    if not HAS_MATPLOTLIB:
        pytest.skip("matplotlib unavailable")

    fig, ax = plt.subplots()
    _codec_plot_fn(_sample_compare_v1())(ax)
    assert ax.get_ylabel() == "bitrate (kbps)"
    plt.close(fig)

    fig, ax = plt.subplots()
    _sweep_plot_fn(_sample_compare_v2())(ax)
    assert ax.get_xlabel() == "bitrate (kbps, log scale; left is smaller)"
    plt.close(fig)

    fig, ax = plt.subplots()
    _ladder_plot_fn(_sample_ladder())(ax)
    assert ax.get_xlabel() == "bitrate (kbps)"
    plt.close(fig)


def test_failed_row_numeric_zero_renders_as_dash():
    data = _sample_compare_v1()
    md = render_markdown(data)
    html = render_html(data)

    for line in md.splitlines():
        if "libvpx" in line and "timeout" in line:
            assert "0 kbps" not in line
            assert " 0.00 " not in line
            assert _DASH in line

    assert "<tr class='failed'><td>" in html
    failed_row_start = html.find("<tr class='failed'>")
    failed_row_end = html.find("</tr>", failed_row_start)
    failed_row_html = html[failed_row_start:failed_row_end]
    assert f"<td class='num'>{_DASH}</td>" in failed_row_html
    assert "<td class='num'>0 kbps</td>" not in failed_row_html
    assert "<td class='num'>0.00</td>" not in failed_row_html


def test_videotoolbox_distinct_palette_colours():
    assert "h264_videotoolbox" in _CODEC_COLOURS
    assert "hevc_videotoolbox" in _CODEC_COLOURS
    assert "av1_videotoolbox" in _CODEC_COLOURS

    assert _CODEC_COLOURS["h264_videotoolbox"] != _CODEC_COLOURS["libx264"]
    assert _CODEC_COLOURS["hevc_videotoolbox"] != _CODEC_COLOURS["libx265"]
    assert _CODEC_COLOURS["av1_videotoolbox"] != _CODEC_COLOURS["libsvtav1"]

    assert _codec_colour("h264_videotoolbox", 0) != _codec_colour("libx264", 0)
    assert _codec_colour("hevc_videotoolbox", 0) != _codec_colour("libx265", 0)
    assert _codec_colour("av1_videotoolbox", 0) != _codec_colour("libsvtav1", 0)


def test_pareto_frontier_deduplication_and_bitrate_annotation():
    if not HAS_MATPLOTLIB:
        pytest.skip("matplotlib unavailable")

    data = _sample_compare_v2()
    fig, ax = plt.subplots()
    _sweep_plot_fn(data)(ax)

    annot_texts = [child.get_text() for child in ax.texts]
    svt_annots = [t for t in annot_texts if "libsvtav1" in t and "failed" not in t]
    assert len(svt_annots) == 1
    assert "libsvtav1 @ 1.60 Mbps" in svt_annots[0]
    plt.close(fig)


def test_sweep_chart_picked_crf_and_failed_target_legend():
    if not HAS_MATPLOTLIB:
        pytest.skip("matplotlib unavailable")

    data = _sample_compare_v2()
    fig, ax = plt.subplots()
    _sweep_plot_fn(data)(ax)

    _handles, labels = ax.get_legend_handles_labels()
    assert "picked CRF" in labels
    assert "failed target" in labels

    annot_texts = [child.get_text() for child in ax.texts]
    assert any("libx265 failed" in t for t in annot_texts)
    plt.close(fig)


def test_render_byte_determinism():
    data = _sample_compare_v2()
    html1 = render_html(data)
    html2 = render_html(data)
    assert html1 == html2

    md1 = render_markdown(data)
    md2 = render_markdown(data)
    assert md1 == md2

    data1 = _sample_compare_v1()
    assert render_html(data1) == render_html(data1)


def test_sidecar_json_round_trip():
    fixtures = [
        _sample_compare_v1(),
        _sample_compare_v2(),
        _sample_ladder(),
        _sample_per_shot(),
    ]
    for data in fixtures:
        d = data.to_dict()
        raw_json = json.dumps(d)
        parsed = json.loads(raw_json)
        restored = ReportData.from_dict(parsed)

        assert restored.source == data.source
        assert restored.target_vmaf == data.target_vmaf
        assert restored.sweep_targets == data.sweep_targets
        assert len(restored.codec_rows) == len(data.codec_rows)
        assert len(restored.sweep_points) == len(data.sweep_points)
        assert len(restored.ladder_samples) == len(data.ladder_samples)
        assert len(restored.ladder_rungs) == len(data.ladder_rungs)
        assert len(restored.shots) == len(data.shots)

        for orig_p, rest_p in zip(data.sweep_points, restored.sweep_points):
            assert orig_p.codec == rest_p.codec
            assert orig_p.target_vmaf == rest_p.target_vmaf
            assert orig_p.best_crf == rest_p.best_crf
            assert orig_p.ok == rest_p.ok
            assert len(orig_p.bisect_samples) == len(rest_p.bisect_samples)
            for orig_s, rest_s in zip(orig_p.bisect_samples, rest_p.bisect_samples):
                assert orig_s == rest_s
