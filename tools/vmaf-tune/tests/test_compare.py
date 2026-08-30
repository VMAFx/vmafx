# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Codec-comparison smoke tests (research-0061 Bucket #7).

The recommend predicate is mocked; no ffmpeg / vmaf binaries required.
"""

from __future__ import annotations

import csv
import io
import json
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.compare import (
    COMPARE_ROW_KEYS,
    ComparisonReport,
    RecommendResult,
    compare_codecs,
    default_encoders,
    emit_report,
    supported_formats,
)

# Synthetic per-codec results: x265 wins on bitrate, libaom slowest, x264
# baseline, svtav1 mid-pack. Numbers are illustrative, not measured.
_FAKE_TABLE: dict[str, RecommendResult] = {
    "libx264": RecommendResult(
        codec="libx264",
        best_crf=23,
        bitrate_kbps=2400.0,
        encode_time_ms=1500.0,
        vmaf_score=92.1,
        encoder_version="libx264-164",
    ),
    "libx265": RecommendResult(
        codec="libx265",
        best_crf=26,
        bitrate_kbps=1700.0,
        encode_time_ms=4200.0,
        vmaf_score=92.0,
        encoder_version="libx265-3.5",
    ),
    "libsvtav1": RecommendResult(
        codec="libsvtav1",
        best_crf=32,
        bitrate_kbps=1900.0,
        encode_time_ms=2800.0,
        vmaf_score=92.3,
        encoder_version="libsvtav1-1.7.0",
    ),
    "libaom": RecommendResult(
        codec="libaom",
        best_crf=30,
        bitrate_kbps=1500.0,
        encode_time_ms=18000.0,
        vmaf_score=92.4,
        encoder_version="libaom-3.8.0",
    ),
}


def _fake_predicate(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
    if codec not in _FAKE_TABLE:
        return RecommendResult(
            codec=codec,
            best_crf=-1,
            bitrate_kbps=float("nan"),
            encode_time_ms=float("nan"),
            vmaf_score=float("nan"),
            ok=False,
            error=f"no fake adapter for {codec!r}",
        )
    return _FAKE_TABLE[codec]


def test_default_encoders_tracks_registry():
    # The default codec set follows the registry so adapter PRs
    # auto-extend the CLI default. Tracks whichever codecs are
    # registered today (libx264 + the NVENC / AMF / QSV / VVenC /
    # SVT-AV1 / VideoToolbox families since the original assertion
    # was written) — assert membership rather than equality so the
    # test stays robust against future adapter additions.
    encoders = default_encoders()
    assert "libx264" in encoders
    assert len(encoders) >= 1


def test_compare_codecs_sorts_by_bitrate():
    report = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=92.0,
        encoders=("libx264", "libx265", "libsvtav1", "libaom"),
        predicate=_fake_predicate,
    )
    ranked_codecs = [r.codec for r in report.rows]
    # Smallest bitrate wins: libaom (1500) < libx265 (1700) < svtav1 (1900) < x264 (2400).
    assert ranked_codecs == ["libaom", "libx265", "libsvtav1", "libx264"]
    assert report.best() is not None
    assert report.best().codec == "libaom"
    assert report.target_vmaf == 92.0
    assert report.tool_version  # non-empty


def test_compare_codecs_serial_matches_parallel():
    encoders = ("libx264", "libx265", "libsvtav1", "libaom")
    par = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=92.0,
        encoders=encoders,
        predicate=_fake_predicate,
        parallel=True,
    )
    seq = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=92.0,
        encoders=encoders,
        predicate=_fake_predicate,
        parallel=False,
    )
    assert [r.codec for r in par.rows] == [r.codec for r in seq.rows]


def test_compare_codecs_unknown_codec_carries_error():
    report = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=92.0,
        encoders=("libx264", "libfoobar"),
        predicate=_fake_predicate,
    )
    by_codec = {r.codec: r for r in report.rows}
    assert by_codec["libx264"].ok is True
    assert by_codec["libfoobar"].ok is False
    assert "no fake adapter" in by_codec["libfoobar"].error
    # Failed rows trail successful ones in the ranking.
    assert report.rows[-1].codec == "libfoobar"


def test_compare_codecs_predicate_exception_is_captured():
    def boom(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
        if codec == "libx264":
            return _FAKE_TABLE["libx264"]
        raise RuntimeError(f"adapter for {codec} crashed")

    report = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=92.0,
        encoders=("libx264", "libx265"),
        predicate=boom,
    )
    by_codec = {r.codec: r for r in report.rows}
    assert by_codec["libx264"].ok is True
    assert by_codec["libx265"].ok is False
    assert "RuntimeError" in by_codec["libx265"].error


def test_compare_codecs_empty_encoder_list_raises():
    with pytest.raises(ValueError):
        compare_codecs(
            src=Path("ref.yuv"),
            target_vmaf=92.0,
            encoders=(),
            predicate=_fake_predicate,
        )


def test_default_predicate_points_at_make_bisect_predicate():
    # Phase B (target-VMAF bisect) ships in vmaftune.bisect, but the
    # bare (codec, src, target_vmaf) predicate signature does not
    # carry source geometry — operators bind that once via
    # make_bisect_predicate(...) and pass the closure into
    # compare_codecs(predicate=...). The default predicate's error
    # string is a one-step pointer at that entry-point.
    report = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=92.0,
        encoders=("libx264",),
    )
    assert report.rows[0].ok is False
    assert "make_bisect_predicate" in report.rows[0].error
    # No best row when every codec fails.
    assert report.best() is None


def test_emit_report_supported_formats_advertised():
    assert set(supported_formats()) == {"markdown", "json", "csv"}


def test_emit_report_markdown_renders_table_and_winner():
    report = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=92.0,
        encoders=("libx264", "libx265"),
        predicate=_fake_predicate,
    )
    md = emit_report(report, format="markdown")
    assert "| Rank | Codec |" in md
    assert "libx265" in md
    assert "libx264" in md
    assert "Smallest file" in md
    # Winner is libx265 (1700 < 2400).
    assert "libx265" in md.split("Smallest file")[1].splitlines()[0]


def test_emit_report_json_round_trip():
    report = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=92.0,
        encoders=("libx264", "libx265"),
        predicate=_fake_predicate,
    )
    payload = json.loads(emit_report(report, format="json"))
    assert payload["src"] == "ref.yuv"
    assert payload["target_vmaf"] == 92.0
    assert len(payload["rows"]) == 2
    # Same key set as COMPARE_ROW_KEYS.
    for row in payload["rows"]:
        assert set(row.keys()) == set(COMPARE_ROW_KEYS)


def test_emit_report_csv_has_header_and_rows():
    report = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=92.0,
        encoders=("libx264", "libx265"),
        predicate=_fake_predicate,
    )
    text = emit_report(report, format="csv")
    reader = csv.DictReader(io.StringIO(text))
    assert reader.fieldnames == list(COMPARE_ROW_KEYS)
    rows = list(reader)
    assert len(rows) == 2
    assert {r["codec"] for r in rows} == {"libx264", "libx265"}


def test_emit_report_unknown_format_raises():
    report = ComparisonReport(
        src="ref.yuv",
        target_vmaf=92.0,
        tool_version="0.0.1",
        wall_time_ms=0.0,
        rows=(),
    )
    with pytest.raises(ValueError):
        emit_report(report, format="yaml")


def test_cli_compare_stdout_smoke(capsys, monkeypatch, tmp_path):
    """End-to-end CLI smoke through ``--predicate-module``."""
    # Inject a shim module the CLI can import via --predicate-module.
    import types

    shim = types.ModuleType("_compare_shim")

    def predicate(codec, src, target_vmaf):
        return _fake_predicate(codec, src, target_vmaf)

    shim.predicate = predicate  # type: ignore[attr-defined]
    sys.modules["_compare_shim"] = shim

    from vmaftune.cli import main

    rc = main(
        [
            "compare",
            "--src",
            str(tmp_path / "ref.yuv"),
            "--target-vmaf",
            "92",
            "--encoders",
            "libx264,libx265,libsvtav1",
            "--format",
            "csv",
            "--predicate-module",
            "_compare_shim:predicate",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    reader = csv.DictReader(io.StringIO(out))
    assert reader.fieldnames == list(COMPARE_ROW_KEYS)
    codecs = [r["codec"] for r in reader]
    # Ranked by bitrate: libx265 (1700) < libsvtav1 (1900) < libx264 (2400).
    assert codecs == ["libx265", "libsvtav1", "libx264"]


def test_cli_compare_requires_geometry_without_predicate_module(capsys, tmp_path):
    """Default CLI path is real bisect, so source geometry is mandatory."""
    from vmaftune.cli import main

    rc = main(
        [
            "compare",
            "--src",
            str(tmp_path / "ref.yuv"),
            "--target-vmaf",
            "92",
            "--encoders",
            "libx264,libx265",
        ]
    )
    assert rc == 2
    err = capsys.readouterr().err
    assert "--width and --height are required" in err


def test_cli_compare_binds_real_bisect_predicate(monkeypatch, capsys, tmp_path):
    """Geometry flags build the Phase-B bisect predicate for each codec."""
    from vmaftune import cli as cli_module

    captured: list[dict] = []

    def fake_make_bisect_predicate(**kwargs):
        captured.append(kwargs)

        def predicate(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
            return RecommendResult(
                codec=codec,
                best_crf=23 if codec == "libx264" else 27,
                bitrate_kbps=2400.0 if codec == "libx264" else 1700.0,
                encode_time_ms=100.0,
                vmaf_score=target_vmaf,
                encoder_version=f"{codec}-fake",
            )

        return predicate

    monkeypatch.setattr(
        "vmaftune.bisect.make_bisect_predicate",
        fake_make_bisect_predicate,
    )

    rc = cli_module.main(
        [
            "compare",
            "--src",
            str(tmp_path / "ref.yuv"),
            "--target-vmaf",
            "92",
            "--encoders",
            "libx264,libx265",
            "--width",
            "1920",
            "--height",
            "1080",
            "--framerate",
            "24",
            "--duration",
            "10",
            "--sample-clip-seconds",
            "4",
            "--crf-min",
            "15",
            "--crf-max",
            "40",
            "--format",
            "json",
        ]
    )
    assert rc == 0
    # ADR-0577 added ``decode_semaphore`` to the kwargs; check all other
    # keys explicitly and then verify decode_semaphore is a Semaphore(1).
    assert len(captured) == 1
    kwargs = captured[0]
    import threading as _threading

    decode_sem = kwargs.pop("decode_semaphore", None)
    assert isinstance(
        decode_sem, _threading.Semaphore
    ), f"Expected decode_semaphore to be a threading.Semaphore, got {decode_sem!r}"
    # ADR-0624: nr_proxy_backend=None is forwarded when --fast-nr is not passed.
    nr_proxy = kwargs.pop("nr_proxy_backend", "MISSING")
    assert nr_proxy is None, f"Expected nr_proxy_backend=None (no --fast-nr), got {nr_proxy!r}"
    assert kwargs == {
        "target_vmaf": 92.0,
        "width": 1920,
        "height": 1080,
        "pix_fmt": "yuv420p",
        "framerate": 24.0,
        "duration_s": 10.0,
        "sample_clip_seconds": 4.0,
        "preset": "medium",  # ADR-1077: default changed from None to "medium"
        "crf_range": (15, 40),
        "max_iterations": 8,
        "vmaf_model": "vmaf_v0.6.1",
        "score_backend": None,
        "ffmpeg_bin": "ffmpeg",
        "vmaf_bin": "vmaf",
        "workdir": None,
    }
    payload = json.loads(capsys.readouterr().out)
    assert [row["codec"] for row in payload["rows"]] == ["libx265", "libx264"]


def test_cli_compare_runtime_variant_binds_per_encoder_ffmpeg(monkeypatch, capsys, tmp_path):
    """``ADAPTER@VARIANT`` rows use the base adapter with a token-local FFmpeg."""
    from vmaftune import cli as cli_module

    captured: list[dict] = []

    def fake_make_bisect_predicate(**kwargs):
        captured.append(dict(kwargs))
        ffmpeg_bin = kwargs["ffmpeg_bin"]

        def predicate(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
            bitrate = 1800.0 if ffmpeg_bin.endswith("ffmpeg-main") else 1600.0
            return RecommendResult(
                codec=codec,
                best_crf=27,
                bitrate_kbps=bitrate,
                encode_time_ms=100.0,
                vmaf_score=target_vmaf,
                encoder_version=f"{codec}:{Path(ffmpeg_bin).name}",
            )

        return predicate

    monkeypatch.setattr(
        "vmaftune.bisect.make_bisect_predicate",
        fake_make_bisect_predicate,
    )

    rc = cli_module.main(
        [
            "compare",
            "--src",
            str(tmp_path / "ref.yuv"),
            "--target-vmaf",
            "92",
            "--encoders",
            "libsvtav1,libsvtav1@svt-av1-hdr",
            "--ffmpeg-bin",
            "/opt/ffmpeg-main",
            "--encoder-ffmpeg-bin",
            "libsvtav1@svt-av1-hdr=/opt/ffmpeg-hdr",
            "--width",
            "1920",
            "--height",
            "1080",
            "--duration",
            "10",
            "--format",
            "json",
            "--no-parallel",
        ]
    )

    assert rc == 0
    assert sorted(call["ffmpeg_bin"] for call in captured) == [
        "/opt/ffmpeg-hdr",
        "/opt/ffmpeg-main",
    ]
    payload = json.loads(capsys.readouterr().out)
    by_codec = {row["codec"]: row for row in payload["rows"]}
    assert set(by_codec) == {"libsvtav1", "libsvtav1@svt-av1-hdr"}
    hdr = by_codec["libsvtav1@svt-av1-hdr"]
    assert hdr["adapter"] == "libsvtav1"
    assert hdr["runtime_variant"] == "svt-av1-hdr"
    assert hdr["ffmpeg_bin"] == "/opt/ffmpeg-hdr"


# -----------------------------------------------------------------------------
# ADR-0509 / BBB e2e v7 Bug #V7-1 regression tests — container-source auto-
# probe of framerate / duration in ``_run_compare``.
#
# Root cause: the compare CLI threaded ``--framerate`` (argparse default
# 24.0) and ``--duration`` (default 0.0) into ``make_bisect_predicate``
# verbatim. When ``--src`` is a 60 fps container (BBB, e.g.), the per-
# iteration ``frame_skip_ref`` / ``frame_cnt`` derived from the default
# 24 fps no longer indexes the same frames the encoder pulled — the
# distorted MKV is decoded back to YUV at the container's native rate
# while ``vmaf --frame_skip_ref`` skips frames at the wrong rate,
# comparing misaligned content and collapsing the apparent VMAF (BBB
# 60 fps → VMAF=90 at CRF=6, physically wrong).
#
# These tests pin the fix at the CLI level: when the user does NOT
# pass ``--framerate`` / ``--duration``, the probed values must reach
# ``make_bisect_predicate``; when the user DOES pass them, the
# override wins (with an explicit-mismatch stderr warning).
# -----------------------------------------------------------------------------


def _stub_probe_source(*, fps: float, duration: float, width: int = 1920, height: int = 1080):
    """Return a stub ``probe_source`` returning the given fps / duration."""
    from dataclasses import dataclass

    @dataclass
    class _Info:
        path: str
        width: int
        height: int
        fps: float
        duration_s: float
        frame_count: int = 0
        codec: str = "h264"
        size_bytes: int = 0

    def _probe(path: Path) -> _Info:
        return _Info(
            path=str(path),
            width=width,
            height=height,
            fps=fps,
            duration_s=duration,
        )

    return _probe


def test_resolve_compare_source_geometry_probes_container_when_default_framerate(
    tmp_path,
):
    """Container src + default --framerate => probe wins."""
    from vmaftune.cli import _resolve_compare_source_geometry

    src = tmp_path / "bbb.mp4"
    src.touch()
    stream = io.StringIO()
    w, h, fr, dur = _resolve_compare_source_geometry(
        src,
        width=1920,
        height=1080,
        framerate=24.0,  # argparse default
        duration_s=0.0,  # argparse default
        framerate_was_default=True,
        duration_was_default=True,
        probe_fn=_stub_probe_source(fps=60.0, duration=634.5),
        warn_stream=stream,
    )
    assert (w, h) == (1920, 1080)
    assert fr == pytest.approx(60.0)
    assert dur == pytest.approx(634.5)
    # No warning when the user didn't pass conflicting flags.
    assert stream.getvalue() == ""


def test_resolve_compare_source_geometry_explicit_user_override_wins_with_warning(
    tmp_path,
):
    """Container src + explicit --framerate mismatch => keep user, warn."""
    from vmaftune.cli import _resolve_compare_source_geometry

    src = tmp_path / "bbb.mp4"
    src.touch()
    stream = io.StringIO()
    _w, _h, fr, _dur = _resolve_compare_source_geometry(
        src,
        width=1920,
        height=1080,
        framerate=24.0,  # user explicitly passed 24 — keep it
        duration_s=0.0,
        framerate_was_default=False,
        duration_was_default=True,
        probe_fn=_stub_probe_source(fps=60.0, duration=634.5),
        warn_stream=stream,
    )
    assert fr == pytest.approx(24.0), "user override must win"
    warn = stream.getvalue()
    assert "disagrees with the probed source rate 60 fps" in warn
    assert "24" in warn


def test_resolve_compare_source_geometry_raw_yuv_skips_probe(tmp_path):
    """Raw YUV src => no probe call, user values verbatim."""
    from vmaftune.cli import _resolve_compare_source_geometry

    src = tmp_path / "ref.yuv"
    src.touch()
    probe_calls: list[Path] = []

    def _probe(p: Path):
        probe_calls.append(p)
        raise AssertionError("probe should not be called for raw YUV")

    stream = io.StringIO()
    w, h, fr, dur = _resolve_compare_source_geometry(
        src,
        width=640,
        height=360,
        framerate=24.0,
        duration_s=0.0,
        framerate_was_default=True,
        duration_was_default=True,
        probe_fn=_probe,
        warn_stream=stream,
    )
    assert (w, h, fr, dur) == (640, 360, 24.0, 0.0)
    assert probe_calls == []
    assert stream.getvalue() == ""


def test_resolve_compare_source_geometry_fills_width_height_from_probe(tmp_path):
    """Container src + no --width/--height => probe fills geometry."""
    from vmaftune.cli import _resolve_compare_source_geometry

    src = tmp_path / "bbb.mp4"
    src.touch()
    w, h, fr, dur = _resolve_compare_source_geometry(
        src,
        width=None,
        height=None,
        framerate=24.0,
        duration_s=0.0,
        framerate_was_default=True,
        duration_was_default=True,
        probe_fn=_stub_probe_source(fps=60.0, duration=5.0, width=3840, height=2160),
    )
    assert (w, h) == (3840, 2160)
    assert fr == pytest.approx(60.0)
    assert dur == pytest.approx(5.0)


def test_resolve_compare_source_geometry_probe_failure_is_best_effort(tmp_path):
    """Probe raises => user-supplied geometry kept verbatim + stderr warning."""
    from vmaftune.cli import _resolve_compare_source_geometry

    src = tmp_path / "bbb.mp4"
    src.touch()

    def _probe(p: Path):
        raise OSError("ffprobe missing")

    stream = io.StringIO()
    w, h, fr, dur = _resolve_compare_source_geometry(
        src,
        width=1920,
        height=1080,
        framerate=24.0,
        duration_s=0.0,
        framerate_was_default=True,
        duration_was_default=True,
        probe_fn=_probe,
        warn_stream=stream,
    )
    assert (w, h, fr, dur) == (1920, 1080, 24.0, 0.0)
    assert "ffprobe" in stream.getvalue() and "failed" in stream.getvalue()


def test_cli_compare_passes_probed_framerate_for_container_src(monkeypatch, capsys, tmp_path):
    """End-to-end: ``compare --src foo.mp4`` (no --framerate) => make_bisect_predicate sees probed fps.

    This is the CLI-level pin of the ADR-0509 / V7-1 fix: a container
    source with default --framerate must reach the bisect predicate
    with the probed source rate (60 fps for the stubbed BBB), not the
    argparse default 24 fps. Sister test to ``test_cli_compare_binds_real_bisect_predicate``
    which covers the raw-YUV path.
    """
    from vmaftune import cli as cli_module

    captured: list[dict] = []

    def fake_make_bisect_predicate(**kwargs):
        captured.append(kwargs)

        def predicate(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
            return RecommendResult(
                codec=codec,
                best_crf=28,
                bitrate_kbps=1000.0,
                encode_time_ms=100.0,
                vmaf_score=target_vmaf,
                encoder_version=f"{codec}-fake",
            )

        return predicate

    monkeypatch.setattr(
        "vmaftune.bisect.make_bisect_predicate",
        fake_make_bisect_predicate,
    )
    # Stub probe_source so the test doesn't shell out to ffprobe.
    monkeypatch.setattr(
        "vmaftune.report.probe_source",
        _stub_probe_source(fps=60.0, duration=634.5, width=1920, height=1080),
    )

    src = tmp_path / "bbb.mp4"
    src.touch()
    rc = cli_module.main(
        [
            "compare",
            "--src",
            str(src),
            "--target-vmaf",
            "92",
            "--encoders",
            "libx264",
            "--width",
            "1920",
            "--height",
            "1080",
            # No --framerate / --duration — auto-probe must fill them.
            "--format",
            "json",
        ]
    )
    assert rc == 0
    assert len(captured) == 1
    # The probed 60 fps must reach the bisect predicate, not the
    # argparse default 24.0.
    assert captured[0]["framerate"] == pytest.approx(60.0)
    assert captured[0]["duration_s"] == pytest.approx(634.5)


def test_cli_compare_explicit_framerate_override_keeps_user_value(monkeypatch, capsys, tmp_path):
    """End-to-end: explicit ``--framerate 30`` keeps user value + warns on mismatch."""
    from vmaftune import cli as cli_module

    captured: list[dict] = []

    def fake_make_bisect_predicate(**kwargs):
        captured.append(kwargs)

        def predicate(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
            return RecommendResult(
                codec=codec,
                best_crf=28,
                bitrate_kbps=1000.0,
                encode_time_ms=100.0,
                vmaf_score=target_vmaf,
            )

        return predicate

    monkeypatch.setattr(
        "vmaftune.bisect.make_bisect_predicate",
        fake_make_bisect_predicate,
    )
    monkeypatch.setattr(
        "vmaftune.report.probe_source",
        _stub_probe_source(fps=60.0, duration=634.5, width=1920, height=1080),
    )

    src = tmp_path / "bbb.mp4"
    src.touch()
    rc = cli_module.main(
        [
            "compare",
            "--src",
            str(src),
            "--target-vmaf",
            "92",
            "--encoders",
            "libx264",
            "--width",
            "1920",
            "--height",
            "1080",
            "--framerate",
            "30",  # explicit override — must win over the 60 fps probe
            "--format",
            "json",
        ]
    )
    assert rc == 0
    assert captured[0]["framerate"] == pytest.approx(30.0)
    err = capsys.readouterr().err
    assert "disagrees with the probed source rate 60 fps" in err
