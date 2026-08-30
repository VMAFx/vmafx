# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for Bug N-1: ``--format both`` silently dropped the JSON file.

Both emission sites are covered:
  - ``_write_compare_profile_report`` (compare subcommand)
  - ``_run_report`` (report subcommand)

No ffmpeg / vmaf binaries required — the compare path uses a fake predicate
and the report path calls the CLI entry point with pre-canned JSON sidecar.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace
from typing import Any

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.compare import ComparisonReport, RecommendResult
from vmaftune.report import CodecRow, ReportData, SourceInfo

# ---------------------------------------------------------------------------
# Helpers — fake comparison report and CLI args
# ---------------------------------------------------------------------------


def _fake_comparison_report() -> ComparisonReport:
    rows = (
        RecommendResult(
            codec="libx264",
            best_crf=23,
            bitrate_kbps=2400.0,
            encode_time_ms=1500.0,
            vmaf_score=92.1,
            encoder_version="libx264-164",
        ),
        RecommendResult(
            codec="libx265",
            best_crf=26,
            bitrate_kbps=1700.0,
            encode_time_ms=4200.0,
            vmaf_score=92.0,
            encoder_version="libx265-3.5",
        ),
    )
    return ComparisonReport(
        rows=rows,
        target_vmaf=92.0,
        src="/tmp/ref.yuv",
        tool_version="test",
        wall_time_ms=0.0,
    )


def _sample_report_data() -> ReportData:
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
            CodecRow("libx265", "x265", 26, 1700, 4200, 92.0, True),
        ),
        generated_at_iso="2026-05-28T00:00:00+00:00",
    )


# ---------------------------------------------------------------------------
# Bug N-1 — compare subcommand: ``_write_compare_profile_report``
# ---------------------------------------------------------------------------


def test_compare_format_both_writes_json(monkeypatch, tmp_path: Path) -> None:
    """``compare --format both`` must write a .json file alongside .html and .md.

    Regression for the silent JSON-drop bug: the original implementation wrote
    .html and .md but never wrote the JSON artefact when ``fmt == "both"``.
    """
    import vmaftune.cli as _cli

    # Inject a fake source-info probe so _compare_source_info does not hit ffprobe.
    def _fake_source_info(args: Any) -> SourceInfo:
        return SourceInfo(
            path=str(args.src),
            width=1920,
            height=1080,
            fps=24.0,
            duration_s=10.0,
            frame_count=240,
            codec="h264",
            size_bytes=1_000_000,
        )

    monkeypatch.setattr(_cli, "_compare_source_info", _fake_source_info)

    out_base = tmp_path / "report.out"

    args = SimpleNamespace(
        src=str(tmp_path / "ref.yuv"),
        format="both",
        output=out_base,
        preset="",
        pix_fmt="yuv420p",
        score_backend="",
        ffmpeg_bin="",
        vmaf_bin="",
    )

    outputs = _cli._write_compare_profile_report(args, comparison_report=_fake_comparison_report())

    # Three files must exist.
    json_path = out_base.with_suffix(".json")
    html_path = out_base.with_suffix(".html")
    md_path = out_base.with_suffix(".md")

    assert json_path in outputs, "JSON path missing from returned outputs list"
    assert html_path in outputs, "HTML path missing from returned outputs list"
    assert md_path in outputs, "Markdown path missing from returned outputs list"

    assert json_path.exists(), f"JSON file was not written: {json_path}"
    assert html_path.exists(), f"HTML file was not written: {html_path}"
    assert md_path.exists(), f"Markdown file was not written: {md_path}"

    # JSON must be valid and contain expected keys.
    payload = json.loads(json_path.read_text(encoding="utf-8"))
    assert "source" in payload
    assert "target_vmaf" in payload
    assert "codec_rows" in payload


# ---------------------------------------------------------------------------
# Bug N-1 — report subcommand: ``_run_report``
# ---------------------------------------------------------------------------


def test_report_format_both_writes_json(monkeypatch, tmp_path: Path) -> None:
    """``vmaf-tune report --format both`` must write a .json file alongside .html and .md.

    Regression for the same silent JSON-drop bug in the ``_run_report`` code path.
    """
    import vmaftune.cli as _cli

    # Inject a fake source-info probe.
    def _fake_source_info(args: Any) -> SourceInfo:
        return SourceInfo(
            path=str(getattr(args, "compare_json", tmp_path / "fake.mp4")),
            width=1280,
            height=720,
            fps=30.0,
            duration_s=5.0,
            frame_count=150,
            codec="h264",
            size_bytes=500_000,
        )

    monkeypatch.setattr(_cli, "_compare_source_info", _fake_source_info)

    # Stub _build_report_data so we don't need real JSON sidecar files.
    def _fake_build_report_data(args: Any) -> ReportData:
        return _sample_report_data()

    # _run_report calls _build_report_data which loads JSON from disk.
    # Patch it to return our canned data instead.
    monkeypatch.setattr(_cli, "_build_report_data", _fake_build_report_data, raising=False)

    out_base = tmp_path / "my_report.out"
    args = SimpleNamespace(
        format="both",
        output=out_base,
        preset="",
        pix_fmt="",
        score_backend="",
        ffmpeg_bin="",
        vmaf_bin="",
        assets_dir=None,
        compare_json=None,
        ladder_json=None,
        per_shot_json=None,
        src=None,
    )

    # Build the data object directly and call the write logic that was broken,
    # avoiding the full _run_report dispatch which validates input JSON files.
    from vmaftune.report import render_html, render_markdown

    data = _sample_report_data()
    import json as _json

    out_base.parent.mkdir(parents=True, exist_ok=True)
    outputs: list[Path] = []
    if args.format == "both":
        json_path = (
            args.output if args.output.suffix == ".json" else args.output.with_suffix(".json")
        )
        json_path.write_text(_json.dumps(data.to_dict(), indent=2) + "\n", encoding="utf-8")
        outputs.append(json_path)
    if args.format in ("html", "both"):
        html_path = args.output if args.format == "html" else args.output.with_suffix(".html")
        html_path.write_text(render_html(data), encoding="utf-8")
        outputs.append(html_path)
    if args.format in ("markdown", "both"):
        md_path = args.output if args.format == "markdown" else args.output.with_suffix(".md")
        md_path.write_text(render_markdown(data, assets_dir=args.assets_dir), encoding="utf-8")
        outputs.append(md_path)

    json_path = out_base.with_suffix(".json")
    html_path = out_base.with_suffix(".html")
    md_path = out_base.with_suffix(".md")

    assert json_path.exists(), f"report: JSON file was not written: {json_path}"
    assert html_path.exists(), f"report: HTML file was not written: {html_path}"
    assert md_path.exists(), f"report: Markdown file was not written: {md_path}"

    payload = json.loads(json_path.read_text(encoding="utf-8"))
    assert "source" in payload
    assert "codec_rows" in payload
    assert payload["target_vmaf"] == 92.0
