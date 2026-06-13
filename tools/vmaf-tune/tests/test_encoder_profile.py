# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for report-backed encoder profile execution."""

from __future__ import annotations

import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

from vmaftune.cli import main
from vmaftune.encoder_profile import (
    build_encode_request,
    load_profile_payload,
    select_recommendation,
)
from vmaftune.report import CodecSweepPoint, ReportData, SourceInfo, render_html, render_markdown


def _profile_data() -> ReportData:
    return ReportData(
        source=SourceInfo(
            path="/profiles/source.mkv",
            width=1920,
            height=1080,
            fps=24.0,
            duration_s=10.0,
            frame_count=240,
            codec="h264",
            size_bytes=1_000_000,
        ),
        target_vmaf=94.0,
        sweep_targets=(94.0, 96.0),
        sweep_points=(
            CodecSweepPoint("libx265", "x265", 94.0, 24, 2200, 1000, 94.3, True),
            CodecSweepPoint("libsvtav1", "SVT-AV1", 94.0, 30, 1600, 1200, 94.2, True),
            CodecSweepPoint("libsvtav1", "SVT-AV1", 96.0, 26, 2500, 1400, 96.2, True),
        ),
        pix_fmt="yuv420p10le",
        ffmpeg_bin="ffmpeg-profile",
        vmaf_bin="vmaf-profile",
    )


def test_load_profile_from_html_and_markdown(tmp_path):
    data = _profile_data()
    html_path = tmp_path / "profile.html"
    md_path = tmp_path / "profile.md"
    html_path.write_text(render_html(data), encoding="utf-8")
    md_path.write_text(render_markdown(data), encoding="utf-8")

    html_profile = load_profile_payload(html_path)
    md_profile = load_profile_payload(md_path)

    assert html_profile["schema"] == "vmaftune.encoder_profile.v1"
    assert md_profile["schema"] == "vmaftune.encoder_profile.v1"
    assert html_profile["recommendations"][0]["codec"] == "libsvtav1"
    assert md_profile["source"]["path"] == "/profiles/source.mkv"


def test_select_recommendation_and_build_request():
    profile = _profile_data().to_dict()["encoder_profile"]
    rec = select_recommendation(profile, codec="libsvtav1", target_vmaf=96.0)
    req = build_encode_request(
        profile,
        rec,
        output=pathlib.Path("/tmp/out.mkv"),
        source_override=pathlib.Path("/tmp/input.mkv"),
        preset_override="slow",
        extra_params=("-movflags", "+faststart"),
    )

    assert rec["crf"] == 26
    assert req.source == pathlib.Path("/tmp/input.mkv")
    assert req.encoder == "libsvtav1"
    assert req.preset == "slow"
    assert req.crf == 26
    assert req.source_is_container is True
    assert req.extra_params == ("-movflags", "+faststart")


def test_encode_profile_dry_run_cli(tmp_path, capsys):
    profile_path = tmp_path / "profile.json"
    profile_path.write_text(json.dumps(_profile_data().to_dict()), encoding="utf-8")
    out = tmp_path / "out.mkv"

    rc = main(
        [
            "encode-profile",
            "--profile",
            str(profile_path),
            "--src",
            str(tmp_path / "input.mkv"),
            "--output",
            str(out),
            "--codec",
            "libsvtav1",
            "--target-vmaf",
            "96",
            "--extra-ffmpeg-arg=-movflags",
            "--extra-ffmpeg-arg=+faststart",
            "--dry-run",
        ]
    )
    captured = capsys.readouterr()
    payload = json.loads(captured.out)

    assert rc == 0
    assert payload["dry_run"] is True
    assert payload["selected"]["codec"] == "libsvtav1"
    assert payload["selected"]["target_vmaf"] == 96.0
    assert payload["ffmpeg_argv"][0] == "ffmpeg-profile"
    assert "-movflags" in payload["ffmpeg_argv"]
    assert str(out) == payload["ffmpeg_argv"][-1]
