# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Parametrized subprocess-mock tests for encode-profile, compare, and report
CLI sub-commands.

Focus areas:
- ``vmaf-tune encode-profile``: dry-run variants, live-encode path (mocked),
  bad-profile error path, filter-by-codec, filter-by-target-vmaf.
- ``vmaf-tune compare``: format variants (markdown/json/csv/html/both),
  --predicate-module dispatch, error paths (bad encoder list, missing
  geometry, --format both without --output, --no-bisect without --crf-sweep).
- ``vmaf-tune report``: compare-json ingestion (v1 + v2 schema), ladder-json
  ingestion, per-shot-json ingestion, format variants (html/markdown/both),
  bad-json-file error paths.

No real ffmpeg or vmaf binary is ever invoked — subprocess and report
seams are monkey-patched.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import cli as cli_module  # noqa: E402
from vmaftune.cli import main  # noqa: E402
from vmaftune.compare import RecommendResult  # noqa: E402
from vmaftune.encode import EncodeRequest, EncodeResult  # noqa: E402
from vmaftune.report import (  # noqa: E402
    CodecRow,
    CodecSweepPoint,
    LadderRung,
    LadderSample,
    ReportData,
    ShotRow,
    SourceInfo,
    render_html,
)

# ---------------------------------------------------------------------------
# Shared fixtures / helpers
# ---------------------------------------------------------------------------


def _fake_source_info(path: str = "/fake/source.yuv") -> SourceInfo:
    return SourceInfo(
        path=path,
        width=1920,
        height=1080,
        fps=24.0,
        duration_s=10.0,
        frame_count=240,
        codec="rawvideo",
        size_bytes=1_000_000,
    )


def _fake_report_data(*, with_v1_rows: bool = False, with_v2_sweep: bool = False) -> ReportData:
    """Build a minimal ReportData for writing test profile JSON files."""
    codec_rows: tuple[CodecRow, ...] = ()
    sweep_points: tuple[CodecSweepPoint, ...] = ()
    sweep_targets: tuple[float, ...] = ()

    if with_v1_rows:
        codec_rows = (
            CodecRow(
                codec="libx264",
                preset="medium",
                crf=23,
                bitrate_kbps=2400.0,
                vmaf_score=92.1,
                encode_time_ms=1500.0,
                ok=True,
                error="",
                encoder_version="264-164",
            ),
        )

    if with_v2_sweep:
        sweep_targets = (92.0, 95.0)
        sweep_points = (
            CodecSweepPoint("libx265", "x265", 92.0, 26, 1700, 800, 92.0, True),
            CodecSweepPoint("libsvtav1", "SVT-AV1", 92.0, 32, 1900, 900, 92.3, True),
            CodecSweepPoint("libx265", "x265", 95.0, 22, 2100, 1200, 95.1, True),
            CodecSweepPoint("libsvtav1", "SVT-AV1", 95.0, 28, 2300, 1100, 95.2, True),
        )

    return ReportData(
        source=_fake_source_info(),
        target_vmaf=92.0,
        codec_rows=codec_rows,
        sweep_points=sweep_points,
        sweep_targets=sweep_targets,
        ladder_samples=(
            LadderSample(1920, 1080, 3000.0, 95.0, 20),
            LadderSample(1280, 720, 1500.0, 90.0, 26),
        ),
        ladder_rungs=(
            LadderRung(1920, 1080, 3000.0, 95.0, 20),
            LadderRung(1280, 720, 1500.0, 90.0, 26),
        ),
        shots=(ShotRow(0, 0, 240, 1920, 1080, 23, 92.0, 2400.0, 10.0),),
        pix_fmt="yuv420p",
        ffmpeg_bin="ffmpeg-test",
        vmaf_bin="vmaf-test",
    )


def _write_html_profile(tmp_path: Path, *, with_v2_sweep: bool = False) -> Path:
    data = _fake_report_data(with_v2_sweep=with_v2_sweep, with_v1_rows=not with_v2_sweep)
    html_path = tmp_path / "profile.html"
    html_path.write_text(render_html(data), encoding="utf-8")
    return html_path


def _fake_encode_result(
    req: EncodeRequest, *, ffmpeg_bin: str = "ffmpeg", **_: object
) -> EncodeResult:
    return EncodeResult(
        request=req,
        encode_size_bytes=1_234_567,
        encode_time_ms=3210.0,
        encoder_version="libx265-fake",
        ffmpeg_version="n7.0-fake",
        exit_status=0,
        stderr_tail="",
    )


def _fake_encode_result_fail(
    req: EncodeRequest, *, ffmpeg_bin: str = "ffmpeg", **_: object
) -> EncodeResult:
    return EncodeResult(
        request=req,
        encode_size_bytes=0,
        encode_time_ms=0.0,
        encoder_version="",
        ffmpeg_version="",
        exit_status=1,
        stderr_tail="Error opening output file",
    )


def _predicate_table() -> dict[str, RecommendResult]:
    return {
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
    }


def _make_predicate_module(table: dict[str, RecommendResult]):
    """Install a synthetic shim into sys.modules and return its name."""
    import types

    shim = types.ModuleType("_cli_subcommand_shim")

    def predicate(codec: str, src: Any, target_vmaf: float) -> RecommendResult:
        if codec not in table:
            return RecommendResult(
                codec=codec,
                best_crf=-1,
                bitrate_kbps=float("nan"),
                encode_time_ms=float("nan"),
                vmaf_score=float("nan"),
                ok=False,
                error=f"no fake adapter for {codec!r}",
            )
        return table[codec]

    shim.predicate = predicate  # type: ignore[attr-defined]
    sys.modules["_cli_subcommand_shim"] = shim
    return "_cli_subcommand_shim:predicate"


# ---------------------------------------------------------------------------
# encode-profile sub-command
# ---------------------------------------------------------------------------


class TestEncodeProfile:
    """Tests for ``vmaf-tune encode-profile`` via the CLI main() entrypoint."""

    def _make_profile(self, tmp_path: Path) -> Path:
        return _write_html_profile(tmp_path, with_v2_sweep=True)

    @pytest.mark.parametrize(
        "codec,target_vmaf",
        [
            ("libx265", 92.0),
            ("libsvtav1", 92.0),
        ],
    )
    def test_dry_run_selects_correct_recommendation(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture,
        codec: str,
        target_vmaf: float,
    ) -> None:
        """Dry-run for each codec selects the matching recommendation row."""
        profile_path = self._make_profile(tmp_path)
        out_path = tmp_path / "out.mkv"

        rc = main(
            [
                "encode-profile",
                "--profile",
                str(profile_path),
                "--output",
                str(out_path),
                "--codec",
                codec,
                "--target-vmaf",
                str(target_vmaf),
                "--dry-run",
            ]
        )

        assert rc == 0, capsys.readouterr().err
        payload = json.loads(capsys.readouterr().out)
        assert payload["dry_run"] is True
        assert payload["selected"]["codec"] == codec
        assert payload["selected"]["target_vmaf"] == pytest.approx(target_vmaf)

    def test_dry_run_emits_ffmpeg_argv(self, tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
        """Dry-run output always contains an ffmpeg_argv list."""
        profile_path = self._make_profile(tmp_path)
        rc = main(
            [
                "encode-profile",
                "--profile",
                str(profile_path),
                "--output",
                str(tmp_path / "o.mkv"),
                "--codec",
                "libsvtav1",
                "--target-vmaf",
                "92",
                "--dry-run",
            ]
        )
        assert rc == 0
        payload = json.loads(capsys.readouterr().out)
        assert isinstance(payload["ffmpeg_argv"], list)
        assert len(payload["ffmpeg_argv"]) >= 2

    def test_live_encode_mocked_success(
        self,
        tmp_path: Path,
        monkeypatch: pytest.MonkeyPatch,
        capsys: pytest.CaptureFixture,
    ) -> None:
        """Live-encode path calls run_encode and emits a JSON result payload."""
        import vmaftune.encode as encode_mod

        monkeypatch.setattr(encode_mod, "run_encode", _fake_encode_result)

        profile_path = self._make_profile(tmp_path)
        out_path = tmp_path / "encoded.mkv"

        rc = main(
            [
                "encode-profile",
                "--profile",
                str(profile_path),
                "--output",
                str(out_path),
                "--codec",
                "libsvtav1",
                "--target-vmaf",
                "92",
            ]
        )

        assert rc == 0
        payload = json.loads(capsys.readouterr().out)
        assert payload["ok"] is True
        assert payload["exit_status"] == 0
        assert payload["encode_size_bytes"] == 1_234_567
        assert payload["encode_time_ms"] == pytest.approx(3210.0)

    def test_live_encode_mocked_failure_returns_nonzero(
        self,
        tmp_path: Path,
        monkeypatch: pytest.MonkeyPatch,
        capsys: pytest.CaptureFixture,
    ) -> None:
        """When run_encode returns exit_status != 0, main() mirrors that exit code."""
        import vmaftune.encode as encode_mod

        monkeypatch.setattr(encode_mod, "run_encode", _fake_encode_result_fail)

        profile_path = self._make_profile(tmp_path)

        rc = main(
            [
                "encode-profile",
                "--profile",
                str(profile_path),
                "--output",
                str(tmp_path / "out.mkv"),
                "--codec",
                "libsvtav1",
                "--target-vmaf",
                "92",
            ]
        )

        assert rc == 1
        payload = json.loads(capsys.readouterr().out)
        assert payload["ok"] is False
        assert "Error opening" in payload["stderr_tail"]

    def test_missing_profile_file_errors(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """A non-existent --profile emits a user-readable error and returns 2."""
        rc = main(
            [
                "encode-profile",
                "--profile",
                str(tmp_path / "nonexistent.json"),
                "--output",
                str(tmp_path / "out.mkv"),
                "--dry-run",
            ]
        )

        assert rc == 2
        err = capsys.readouterr().err
        assert "encode-profile" in err

    def test_filter_by_target_vmaf_selects_correct_rung(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """--target-vmaf 95 selects the 95-target rung, not the 92 one."""
        profile_path = self._make_profile(tmp_path)
        out_path = tmp_path / "out.mkv"

        rc = main(
            [
                "encode-profile",
                "--profile",
                str(profile_path),
                "--output",
                str(out_path),
                "--codec",
                "libx265",
                "--target-vmaf",
                "95",
                "--dry-run",
            ]
        )

        assert rc == 0
        payload = json.loads(capsys.readouterr().out)
        assert payload["selected"]["target_vmaf"] == pytest.approx(95.0)
        assert payload["selected"]["codec"] == "libx265"


# ---------------------------------------------------------------------------
# compare sub-command — format variants + error paths
# ---------------------------------------------------------------------------


class TestCompareFormats:
    """Parametrized format-variant tests for ``vmaf-tune compare``."""

    def _predicate_spec(self) -> str:
        return _make_predicate_module(_predicate_table())

    @pytest.mark.parametrize("fmt", ["markdown", "json", "csv"])
    def test_compare_formats_succeed(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture,
        fmt: str,
    ) -> None:
        """Each text-based output format produces a non-empty result and rc=0."""
        predicate_spec = self._predicate_spec()

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
                fmt,
                "--predicate-module",
                predicate_spec,
            ]
        )

        assert rc == 0, capsys.readouterr().err
        out = capsys.readouterr().out
        assert len(out) > 0

        if fmt == "json":
            payload = json.loads(out)
            assert "rows" in payload or "results" in payload

        if fmt == "csv":
            lines = out.strip().splitlines()
            assert len(lines) >= 2  # header + at least one data row

    def test_compare_json_rows_sorted_by_bitrate(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """JSON output rows are sorted ascending by bitrate_kbps."""
        predicate_spec = self._predicate_spec()

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
                "json",
                "--predicate-module",
                predicate_spec,
            ]
        )

        assert rc == 0
        payload = json.loads(capsys.readouterr().out)
        rows = payload.get("rows") or payload.get("results") or []
        bitrates = [r["bitrate_kbps"] for r in rows if r.get("ok", True)]
        assert bitrates == sorted(bitrates)

    def test_compare_html_writes_to_output_file(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """--format html writes the profile-card HTML to --output."""
        predicate_spec = self._predicate_spec()
        out_path = tmp_path / "report.html"

        rc = main(
            [
                "compare",
                "--src",
                str(tmp_path / "ref.yuv"),
                "--target-vmaf",
                "92",
                "--encoders",
                "libx264,libx265",
                "--format",
                "html",
                "--output",
                str(out_path),
                "--predicate-module",
                predicate_spec,
            ]
        )

        assert rc == 0, capsys.readouterr().err
        assert out_path.exists()
        content = out_path.read_text(encoding="utf-8")
        assert "<html" in content.lower() or "<!doctype" in content.lower()

    def test_compare_both_format_requires_output(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """``--format both`` without ``--output`` should exit with rc=2."""
        predicate_spec = self._predicate_spec()

        rc = main(
            [
                "compare",
                "--src",
                str(tmp_path / "ref.yuv"),
                "--target-vmaf",
                "92",
                "--encoders",
                "libx264",
                "--format",
                "both",
                "--predicate-module",
                predicate_spec,
            ]
        )

        assert rc == 2
        err = capsys.readouterr().err
        assert "--output" in err

    def test_compare_both_format_writes_html_and_json_sidecar(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """``--format both`` with --output writes .html and .md alongside."""
        predicate_spec = self._predicate_spec()
        out_path = tmp_path / "report.html"

        rc = main(
            [
                "compare",
                "--src",
                str(tmp_path / "ref.yuv"),
                "--target-vmaf",
                "92",
                "--encoders",
                "libx264,libx265",
                "--format",
                "both",
                "--output",
                str(out_path),
                "--predicate-module",
                predicate_spec,
            ]
        )

        assert rc == 0, capsys.readouterr().err
        # "both" writes .html + .md
        assert out_path.with_suffix(".html").exists()
        assert out_path.with_suffix(".md").exists()

    def test_compare_empty_encoder_list_errors(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """An empty --encoders string causes a user-readable error and rc=2."""
        predicate_spec = self._predicate_spec()

        rc = main(
            [
                "compare",
                "--src",
                str(tmp_path / "ref.yuv"),
                "--target-vmaf",
                "92",
                "--encoders",
                "  ,  ",
                "--predicate-module",
                predicate_spec,
            ]
        )

        assert rc == 2

    def test_compare_missing_geometry_without_predicate_module_errors(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """Real bisect path requires --width and --height; missing both → rc=2."""
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
        assert "--width" in err or "--height" in err or "required" in err.lower()

    def test_compare_no_bisect_without_crf_sweep_errors(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """--no-bisect requires --crf-sweep; omitting it should exit non-zero."""
        rc = main(
            [
                "compare",
                "--src",
                str(tmp_path / "ref.yuv"),
                "--no-bisect",
                "--encoders",
                "libx264",
                "--width",
                "1920",
                "--height",
                "1080",
                "--framerate",
                "24",
                "--duration",
                "10",
            ]
        )

        # Expect non-zero — the CLI should reject --no-bisect without --crf-sweep.
        assert rc != 0

    @pytest.mark.parametrize(
        "extra_flag,extra_val",
        [
            ("--crf-min", "20"),
            ("--crf-max", "40"),
        ],
    )
    def test_compare_crf_bound_alone_errors(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture,
        extra_flag: str,
        extra_val: str,
    ) -> None:
        """Passing only one of --crf-min/--crf-max (not both) → rc=2."""
        rc = main(
            [
                "compare",
                "--src",
                str(tmp_path / "ref.yuv"),
                "--target-vmaf",
                "92",
                "--encoders",
                "libx264",
                "--width",
                "1920",
                "--height",
                "1080",
                "--framerate",
                "24",
                "--duration",
                "10",
                extra_flag,
                extra_val,
            ]
        )

        assert rc == 2
        err = capsys.readouterr().err
        assert "--crf-min" in err or "--crf-max" in err or "both" in err.lower()

    def test_compare_predicate_module_unknown_encoder_captured(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """An unknown codec in --encoders results in an error row, not a crash."""
        predicate_spec = self._predicate_spec()

        rc = main(
            [
                "compare",
                "--src",
                str(tmp_path / "ref.yuv"),
                "--target-vmaf",
                "92",
                "--encoders",
                "libx264,unknown_codec_xyz",
                "--format",
                "json",
                "--predicate-module",
                predicate_spec,
            ]
        )

        assert rc == 0  # The run succeeds; the unknown codec row is flagged.
        payload = json.loads(capsys.readouterr().out)
        rows = payload.get("rows") or payload.get("results") or []
        error_rows = [r for r in rows if not r.get("ok", True)]
        assert any("unknown_codec_xyz" in r.get("codec", "") for r in error_rows)


# ---------------------------------------------------------------------------
# report sub-command — JSON ingestion + format variants
# ---------------------------------------------------------------------------


def _compare_v1_json() -> dict:
    """Minimal schema-v1 compare JSON (one-target, codec rows)."""
    return {
        "schema_version": 1,
        "target_vmaf": 92.0,
        "rows": [
            {
                "codec": "libx264",
                "preset": "medium",
                "crf": 23,
                "bitrate_kbps": 2400.0,
                "vmaf_score": 92.1,
                "encode_time_ms": 1500.0,
                "ok": True,
                "error": "",
                "encoder_version": "264-164",
            },
            {
                "codec": "libx265",
                "preset": "medium",
                "crf": 26,
                "bitrate_kbps": 1700.0,
                "vmaf_score": 92.0,
                "encode_time_ms": 4200.0,
                "ok": True,
                "error": "",
                "encoder_version": "265-3.5",
            },
        ],
    }


def _compare_v2_json() -> dict:
    """Minimal schema-v2 compare JSON (multi-target sweep, CodecSweepPoint rows)."""
    return {
        "schema_version": 2,
        "target_vmafs": [92.0, 95.0],
        "rows": [
            {
                "codec": "libx265",
                "encoder_version_label": "x265",
                "target_vmaf": 92.0,
                "crf": 26,
                "bitrate_kbps": 1700.0,
                "encode_time_ms": 800.0,
                "vmaf_score": 92.0,
                "ok": True,
                "error": "",
            },
            {
                "codec": "libsvtav1",
                "encoder_version_label": "SVT-AV1",
                "target_vmaf": 92.0,
                "crf": 32,
                "bitrate_kbps": 1900.0,
                "encode_time_ms": 900.0,
                "vmaf_score": 92.3,
                "ok": True,
                "error": "",
            },
            {
                "codec": "libx265",
                "encoder_version_label": "x265",
                "target_vmaf": 95.0,
                "crf": 22,
                "bitrate_kbps": 2100.0,
                "encode_time_ms": 1200.0,
                "vmaf_score": 95.1,
                "ok": True,
                "error": "",
            },
        ],
    }


def _ladder_json() -> dict:
    return {
        "samples": [
            {"width": 1920, "height": 1080, "bitrate_kbps": 3000, "vmaf": 95.0, "crf": 20},
            {"width": 1280, "height": 720, "bitrate_kbps": 1500, "vmaf": 90.0, "crf": 26},
        ],
        "renditions": [
            {"width": 1920, "height": 1080, "bitrate_kbps": 3000, "vmaf": 95.0, "crf": 20},
        ],
    }


def _per_shot_json() -> dict:
    return {
        "framerate": 24.0,
        "shots": [
            {
                "shot_id": 0,
                "start_frame": 0,
                "end_frame": 120,
                "frames": 120,
                "predicted_crf": 23,
                "predicted_vmaf": 92.5,
            },
            {
                "shot_id": 1,
                "start_frame": 120,
                "end_frame": 240,
                "frames": 120,
                "predicted_crf": 25,
                "predicted_vmaf": 91.8,
            },
        ],
    }


def _stub_probe_source(path: Path) -> SourceInfo:
    return _fake_source_info(str(path))


class TestReportSubcommand:
    """CLI tests for ``vmaf-tune report``."""

    @pytest.fixture(autouse=True)
    def _patch_probe_source(self, monkeypatch: pytest.MonkeyPatch) -> None:
        """Stub out probe_source so no ffprobe binary is required."""
        monkeypatch.setattr(cli_module, "probe_source", _stub_probe_source, raising=False)
        import vmaftune.report as report_mod

        monkeypatch.setattr(report_mod, "probe_source", _stub_probe_source)

    def _fake_src(self, tmp_path: Path) -> Path:
        src = tmp_path / "source.yuv"
        src.write_bytes(b"")
        return src

    @pytest.mark.parametrize("fmt", ["html", "markdown"])
    def test_report_basic_formats_succeed(
        self, tmp_path: Path, capsys: pytest.CaptureFixture, fmt: str
    ) -> None:
        """Report renders without error for each basic format when no JSON is provided."""
        out_path = tmp_path / f"report.{fmt[:4]}"

        rc = main(
            [
                "report",
                "--src",
                str(self._fake_src(tmp_path)),
                "--format",
                fmt,
                "--output",
                str(out_path),
            ]
        )

        assert rc == 0, capsys.readouterr().err
        stdout = capsys.readouterr().out
        payload = json.loads(stdout)
        assert payload["ok"] is True
        assert len(payload["outputs"]) == 1

    def test_report_both_format_writes_html_and_md(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """``--format both`` emits .html and .md files."""
        out_base = tmp_path / "report.html"

        rc = main(
            [
                "report",
                "--src",
                str(self._fake_src(tmp_path)),
                "--format",
                "both",
                "--output",
                str(out_base),
            ]
        )

        assert rc == 0, capsys.readouterr().err
        assert out_base.with_suffix(".html").exists()
        assert out_base.with_suffix(".md").exists()
        payload = json.loads(capsys.readouterr().out)
        assert len(payload["outputs"]) == 2

    @pytest.mark.parametrize(
        "schema_builder,expected_codec_rows,expected_sweep_points",
        [
            (_compare_v1_json, 2, 0),
            (_compare_v2_json, 0, 3),
        ],
    )
    def test_report_ingests_compare_json_schema_variants(
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture,
        schema_builder,
        expected_codec_rows: int,
        expected_sweep_points: int,
    ) -> None:
        """Both v1 and v2 compare JSON schemas are ingested correctly."""
        compare_path = tmp_path / "compare.json"
        compare_path.write_text(json.dumps(schema_builder()), encoding="utf-8")

        out_path = tmp_path / "report.html"

        rc = main(
            [
                "report",
                "--src",
                str(self._fake_src(tmp_path)),
                "--compare-json",
                str(compare_path),
                "--format",
                "html",
                "--output",
                str(out_path),
            ]
        )

        assert rc == 0, capsys.readouterr().err
        payload = json.loads(capsys.readouterr().out)
        assert payload["ok"] is True
        assert payload["codec_rows"] == expected_codec_rows
        assert payload["sweep_points"] == expected_sweep_points

    def test_report_ingests_ladder_json(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """Ladder JSON provides sample + rung counts in the output metadata."""
        ladder_path = tmp_path / "ladder.json"
        ladder_path.write_text(json.dumps(_ladder_json()), encoding="utf-8")

        out_path = tmp_path / "report.html"

        rc = main(
            [
                "report",
                "--src",
                str(self._fake_src(tmp_path)),
                "--ladder-json",
                str(ladder_path),
                "--format",
                "html",
                "--output",
                str(out_path),
            ]
        )

        assert rc == 0, capsys.readouterr().err
        payload = json.loads(capsys.readouterr().out)
        assert payload["ladder_samples"] == 2
        assert payload["ladder_rungs"] == 1

    def test_report_ingests_per_shot_json(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """Per-shot JSON provides shot count in the output metadata."""
        per_shot_path = tmp_path / "pershot.json"
        per_shot_path.write_text(json.dumps(_per_shot_json()), encoding="utf-8")

        out_path = tmp_path / "report.html"

        rc = main(
            [
                "report",
                "--src",
                str(self._fake_src(tmp_path)),
                "--per-shot-json",
                str(per_shot_path),
                "--format",
                "html",
                "--output",
                str(out_path),
            ]
        )

        assert rc == 0, capsys.readouterr().err
        payload = json.loads(capsys.readouterr().out)
        assert payload["shots"] == 2

    @pytest.mark.parametrize("bad_flag", ["--compare-json", "--ladder-json", "--per-shot-json"])
    def test_report_bad_json_file_errors(
        self, tmp_path: Path, capsys: pytest.CaptureFixture, bad_flag: str
    ) -> None:
        """A bad/missing JSON input file emits a user-readable error and rc=2."""
        bad_path = tmp_path / "nonexistent.json"

        rc = main(
            [
                "report",
                "--src",
                str(self._fake_src(tmp_path)),
                bad_flag,
                str(bad_path),
                "--format",
                "html",
                "--output",
                str(tmp_path / "report.html"),
            ]
        )

        assert rc == 2
        err = capsys.readouterr().err
        assert "vmaf-tune report" in err

    def test_report_corrupt_compare_json_errors(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """Malformed (non-JSON) compare-json file → rc=2 with readable message."""
        bad_json = tmp_path / "bad.json"
        bad_json.write_text("{{not valid json}}", encoding="utf-8")

        rc = main(
            [
                "report",
                "--src",
                str(self._fake_src(tmp_path)),
                "--compare-json",
                str(bad_json),
                "--format",
                "html",
                "--output",
                str(tmp_path / "report.html"),
            ]
        )

        assert rc == 2
        err = capsys.readouterr().err
        assert "vmaf-tune report" in err

    def test_report_degraded_flag_for_unavailable_encoder_row(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """A v1 codec row with 'encoder unavailable' error sets degraded=true, ok=true."""
        unavail_json = {
            "schema_version": 1,
            "target_vmaf": 92.0,
            "rows": [
                {
                    "codec": "libx264",
                    "preset": "medium",
                    "crf": 23,
                    "bitrate_kbps": 2400.0,
                    "vmaf_score": 92.1,
                    "encode_time_ms": 1500.0,
                    "ok": True,
                    "error": "",
                    "encoder_version": "264-164",
                },
                {
                    "codec": "h264_nvenc",
                    "preset": "",
                    "crf": -1,
                    "bitrate_kbps": 0.0,
                    "vmaf_score": 0.0,
                    "encode_time_ms": 0.0,
                    "ok": False,
                    "error": "encoder unavailable (h264_nvenc): no CUDA device",
                    "encoder_version": "",
                },
            ],
        }
        compare_path = tmp_path / "unavail.json"
        compare_path.write_text(json.dumps(unavail_json), encoding="utf-8")
        out_path = tmp_path / "report.html"

        rc = main(
            [
                "report",
                "--src",
                str(self._fake_src(tmp_path)),
                "--compare-json",
                str(compare_path),
                "--format",
                "html",
                "--output",
                str(out_path),
            ]
        )

        assert rc == 0
        payload = json.loads(capsys.readouterr().out)
        assert payload["ok"] is True
        assert payload["degraded"] is True
        assert payload["codec_rows_unavailable"] == 1

    def test_report_ok_false_for_real_failure_row(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """A v1 codec row with a real (non-unavailable) error sets ok=false."""
        failure_json = {
            "schema_version": 1,
            "target_vmaf": 92.0,
            "rows": [
                {
                    "codec": "libx264",
                    "preset": "medium",
                    "crf": -1,
                    "bitrate_kbps": 0.0,
                    "vmaf_score": 0.0,
                    "encode_time_ms": 0.0,
                    "ok": False,
                    "error": "ffmpeg exited with code 1",
                    "encoder_version": "",
                },
            ],
        }
        compare_path = tmp_path / "failure.json"
        compare_path.write_text(json.dumps(failure_json), encoding="utf-8")
        out_path = tmp_path / "report.html"

        rc = main(
            [
                "report",
                "--src",
                str(self._fake_src(tmp_path)),
                "--compare-json",
                str(compare_path),
                "--format",
                "html",
                "--output",
                str(out_path),
            ]
        )

        assert rc == 0
        payload = json.loads(capsys.readouterr().out)
        assert payload["ok"] is False
        assert payload["degraded"] is False
        assert payload["codec_rows_failed"] == 1
