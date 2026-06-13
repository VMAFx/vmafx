# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Coverage push for vmaftune.encode — gaps identified by inspection.

Covers branches not exercised by the existing encode test suite:

* :func:`build_ffmpeg_command` — sample-clip mode (raw + container),
  duration_s fallback, pass-1 null-muxer redirect, pass-2 output file,
  ``requires stats_path`` guard, ``supports_two_pass=False`` guard.
* :func:`build_pass1_stats_command` — duration_s fallback, container src.
* :func:`run_encode` — pass-1 size skip, rc-failure path, encoder
  version fallback probe via ``_probe_encoder_version_from_ffmpeg``.
* :func:`run_encode_with_stats` — capture_stats=False delegation,
  stats-file parse + result enrichment path.
* :func:`run_two_pass_encode` — on_unsupported="raise" + "fallback",
  unknown value rejection, pass-1 failure short-circuit, combined
  timing and size.
* :func:`bitrate_kbps` — zero duration guard.
* :func:`iter_grid` — basic cartesian product shape.
* :func:`parse_versions` — vpx, svtav1-info-format, auto-detect
  fallback chain, default x264 banner not found.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.encode import (  # noqa: E402
    _PROBE_CACHE,
    EncodeRequest,
    _probe_encoder_version_from_ffmpeg,
    _stats_file_for,
    _tail,
    bitrate_kbps,
    build_ffmpeg_command,
    build_pass1_stats_command,
    iter_grid,
    parse_versions,
    run_encode,
    run_encode_with_stats,
    run_two_pass_encode,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_req(
    *,
    encoder: str = "libx264",
    preset: str = "medium",
    crf: int = 23,
    output: Path | None = None,
    sample_clip_seconds: float = 0.0,
    sample_clip_start_s: float = 0.0,
    duration_s: float = 0.0,
    source_is_container: bool = False,
    pass_number: int = 0,
    stats_path: Path | None = None,
    tmp_path: Path | None = None,
) -> EncodeRequest:
    base = tmp_path or Path("/tmp")
    return EncodeRequest(
        source=base / "ref.yuv",
        width=320,
        height=240,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder=encoder,
        preset=preset,
        crf=crf,
        output=output or (base / "out.mp4"),
        sample_clip_seconds=sample_clip_seconds,
        sample_clip_start_s=sample_clip_start_s,
        duration_s=duration_s,
        source_is_container=source_is_container,
        pass_number=pass_number,
        stats_path=stats_path,
    )


def _fake_runner(
    cmd: list[str],
    *,
    returncode: int = 0,
    stderr: str = "",
    stdout: str = "",
    touch_output: bool = True,
    tmp_path: Path | None = None,
) -> Any:
    """Return a callable that mimics subprocess.run for the given cmd."""

    def _run(c: list[str], capture_output: bool, text: bool, check: bool) -> Any:
        if touch_output and c and tmp_path:
            out = Path(c[-1])
            if not out.name.startswith("-"):
                try:
                    out.parent.mkdir(parents=True, exist_ok=True)
                    out.write_bytes(b"\x00" * 128)
                except OSError:
                    pass
        return type("R", (), {"returncode": returncode, "stdout": stdout, "stderr": stderr})()

    return _run


# ---------------------------------------------------------------------------
# build_ffmpeg_command — sample-clip + duration_s fallback
# ---------------------------------------------------------------------------


class TestBuildFfmpegCommandSampleClip:
    def test_raw_sample_clip_injects_ss_and_t(self, tmp_path: Path) -> None:
        req = _make_req(
            sample_clip_seconds=5.0,
            sample_clip_start_s=10.0,
            tmp_path=tmp_path,
        )
        cmd = build_ffmpeg_command(req)
        assert "-ss" in cmd
        assert cmd[cmd.index("-ss") + 1] == "10.0"
        assert "-t" in cmd
        assert cmd[cmd.index("-t") + 1] == "5.0"
        # -f rawvideo must still be present for raw source
        assert "-f" in cmd
        assert "rawvideo" in cmd

    def test_container_sample_clip_injects_ss_and_t(self, tmp_path: Path) -> None:
        req = _make_req(
            sample_clip_seconds=3.0,
            sample_clip_start_s=2.5,
            source_is_container=True,
            tmp_path=tmp_path,
        )
        cmd = build_ffmpeg_command(req)
        assert "-ss" in cmd
        assert cmd[cmd.index("-ss") + 1] == "2.5"
        assert "-t" in cmd
        assert cmd[cmd.index("-t") + 1] == "3.0"
        # Container source must NOT add -f rawvideo
        assert "rawvideo" not in cmd

    def test_duration_s_fallback_when_no_sample_clip(self, tmp_path: Path) -> None:
        req = _make_req(duration_s=10.0, tmp_path=tmp_path)
        cmd = build_ffmpeg_command(req)
        assert "-t" in cmd
        assert cmd[cmd.index("-t") + 1] == "10.0"
        assert "-ss" not in cmd

    def test_sample_clip_overrides_duration_s(self, tmp_path: Path) -> None:
        req = _make_req(
            sample_clip_seconds=5.0,
            sample_clip_start_s=0.0,
            duration_s=10.0,
            tmp_path=tmp_path,
        )
        cmd = build_ffmpeg_command(req)
        # -t should be the sample_clip_seconds, not duration_s
        t_values = [cmd[i + 1] for i, tok in enumerate(cmd) if tok == "-t"]
        assert "5.0" in t_values
        assert "10.0" not in t_values

    def test_no_clip_no_duration_s(self, tmp_path: Path) -> None:
        req = _make_req(tmp_path=tmp_path)
        cmd = build_ffmpeg_command(req)
        assert "-ss" not in cmd
        assert "-t" not in cmd

    def test_container_duration_s_fallback(self, tmp_path: Path) -> None:
        req = _make_req(duration_s=7.5, source_is_container=True, tmp_path=tmp_path)
        cmd = build_ffmpeg_command(req)
        assert "-t" in cmd
        assert cmd[cmd.index("-t") + 1] == "7.5"


class TestBuildFfmpegCommandTwoPass:
    def test_pass1_redirects_to_null_muxer(self, tmp_path: Path) -> None:
        stats = tmp_path / "stats"
        req = _make_req(
            encoder="libx264",
            pass_number=1,
            stats_path=stats,
            tmp_path=tmp_path,
        )
        cmd = build_ffmpeg_command(req)
        # Pass 1 must write to -f null -
        assert "-f" in cmd
        null_idx = next(
            (i for i in range(len(cmd) - 1) if cmd[i] == "-f" and cmd[i + 1] == "null"),
            None,
        )
        assert null_idx is not None, "expected -f null in pass-1 command"
        assert cmd[-1] == "-"
        # Pass 1 must include -pass 1 from adapter
        assert "-pass" in cmd

    def test_pass2_writes_to_output(self, tmp_path: Path) -> None:
        stats = tmp_path / "stats"
        req = _make_req(
            encoder="libx264",
            pass_number=2,
            stats_path=stats,
            output=tmp_path / "encoded.mp4",
            tmp_path=tmp_path,
        )
        cmd = build_ffmpeg_command(req)
        assert cmd[-1] == str(tmp_path / "encoded.mp4")
        assert "-pass" in cmd
        assert cmd[cmd.index("-pass") + 1] == "2"

    def test_missing_stats_path_raises(self, tmp_path: Path) -> None:
        req = _make_req(encoder="libx264", pass_number=1, stats_path=None, tmp_path=tmp_path)
        with pytest.raises(ValueError, match="stats_path"):
            build_ffmpeg_command(req)

    def test_unsupported_two_pass_raises(self, tmp_path: Path) -> None:
        # libsvtav1 has supports_two_pass=False
        stats = tmp_path / "stats"
        req = _make_req(
            encoder="libsvtav1",
            pass_number=1,
            stats_path=stats,
            tmp_path=tmp_path,
        )
        with pytest.raises(ValueError, match="does not support 2-pass"):
            build_ffmpeg_command(req)


# ---------------------------------------------------------------------------
# build_pass1_stats_command
# ---------------------------------------------------------------------------


class TestBuildPass1StatsCommand:
    def test_raw_source_includes_rawvideo_flags(self, tmp_path: Path) -> None:
        req = _make_req(tmp_path=tmp_path)
        prefix = tmp_path / "stats_prefix"
        cmd = build_pass1_stats_command(req, prefix)
        assert "-f" in cmd
        assert "rawvideo" in cmd
        assert "-pass" in cmd
        assert cmd[cmd.index("-pass") + 1] == "1"
        assert "-passlogfile" in cmd
        assert str(prefix) in cmd

    def test_container_source_omits_rawvideo(self, tmp_path: Path) -> None:
        req = _make_req(source_is_container=True, tmp_path=tmp_path)
        prefix = tmp_path / "stats_prefix"
        cmd = build_pass1_stats_command(req, prefix)
        assert "rawvideo" not in cmd

    def test_duration_s_fallback_in_pass1(self, tmp_path: Path) -> None:
        req = _make_req(duration_s=5.0, tmp_path=tmp_path)
        prefix = tmp_path / "stats_prefix"
        cmd = build_pass1_stats_command(req, prefix)
        assert "-t" in cmd
        assert cmd[cmd.index("-t") + 1] == "5.0"

    def test_sample_clip_in_pass1(self, tmp_path: Path) -> None:
        req = _make_req(
            sample_clip_seconds=3.0,
            sample_clip_start_s=1.0,
            tmp_path=tmp_path,
        )
        prefix = tmp_path / "stats_prefix"
        cmd = build_pass1_stats_command(req, prefix)
        assert "-ss" in cmd
        assert cmd[cmd.index("-ss") + 1] == "1.0"
        assert "-t" in cmd
        assert cmd[cmd.index("-t") + 1] == "3.0"


# ---------------------------------------------------------------------------
# _stats_file_for
# ---------------------------------------------------------------------------


def test_stats_file_for_appends_stream_suffix(tmp_path: Path) -> None:
    prefix = tmp_path / "myprefix"
    result = _stats_file_for(prefix)
    assert result == tmp_path / "myprefix-0.log"


# ---------------------------------------------------------------------------
# parse_versions — extended coverage
# ---------------------------------------------------------------------------


class TestParseVersionsExtended:
    def test_vpx_encoder(self) -> None:
        stderr = "ffmpeg version 6.0\n[libvpx-vp9 @ 0xabc123] v1.13.0\n"
        ffm, enc = parse_versions(stderr, encoder="libvpx-vp9")
        assert ffm == "6.0"
        assert enc == "libvpx-vp9-1.13.0"

    def test_svtav1_newer_format(self) -> None:
        # Newer SVT-AV1 banner: "Svt[info]:SVT-AV1 Encoder Lib v2.1.0"
        stderr = "ffmpeg version 7.0\nSvt[info]:SVT-AV1 Encoder Lib v2.1.0\n"
        ffm, enc = parse_versions(stderr, encoder="libsvtav1")
        assert ffm == "7.0"
        assert enc == "libsvtav1-2.1.0"

    def test_svtav1_vbr_variant(self) -> None:
        stderr = "ffmpeg version 7.0\nSVT-AV1 ENCODER v1.7.0\n"
        ffm, enc = parse_versions(stderr, encoder="libsvtav1-vbr")
        assert ffm == "7.0"
        assert enc.startswith("libsvtav1-")

    def test_hw_nvenc_token_returned_verbatim(self) -> None:
        stderr = "ffmpeg version 6.0\n"
        _, enc = parse_versions(stderr, encoder="hevc_nvenc")
        assert enc == "hevc_nvenc"

    def test_hw_amf_token_returned_verbatim(self) -> None:
        _, enc = parse_versions("ffmpeg version 6.0\n", encoder="h264_amf")
        assert enc == "h264_amf"

    def test_hw_qsv_token_returned_verbatim(self) -> None:
        _, enc = parse_versions("ffmpeg version 6.0\n", encoder="av1_qsv")
        assert enc == "av1_qsv"

    def test_hw_videotoolbox_token_returned_verbatim(self) -> None:
        _, enc = parse_versions("ffmpeg version 6.0\n", encoder="hevc_videotoolbox")
        assert enc == "hevc_videotoolbox"

    def test_completely_unknown_encoder_returns_unknown(self) -> None:
        _, enc = parse_versions("ffmpeg version 6.0\n", encoder="libxyz123_unknown_codec")
        assert enc == "unknown"

    def test_no_ffmpeg_banner_returns_unknown_ffm(self) -> None:
        ffm, _ = parse_versions("no version info here\n")
        assert ffm == "unknown"

    def test_auto_detect_x265_when_default_encoder(self) -> None:
        # No x264 banner present, x265 banner present — auto-detect chain
        stderr = "ffmpeg version 7.0\nx265 [info]: HEVC encoder version 3.5\n"
        ffm, enc = parse_versions(stderr)  # default encoder="libx264"
        assert ffm == "7.0"
        assert enc.startswith("libx265-")

    def test_auto_detect_svtav1_when_no_x264_or_x265_banner(self) -> None:
        stderr = "ffmpeg version 7.0\nSVT-AV1 ENCODER v1.7.0\n"
        ffm, enc = parse_versions(stderr)  # default encoder="libx264"
        assert ffm == "7.0"
        assert enc.startswith("libsvtav1-")

    def test_auto_detect_falls_through_to_unknown(self) -> None:
        stderr = "ffmpeg version 7.0\nno recognisable encoder banner\n"
        _, enc = parse_versions(stderr)
        assert enc == "unknown"

    # --- libaom-av1 (ADR-1077) ---

    def test_libaom_av1_with_version_banner(self) -> None:
        # Older FFmpeg: "[libaom-av1 @ 0xabc] libaom-av1 v3.6.0"
        stderr = "ffmpeg version 7.1\n[libaom-av1 @ 0xabc123] libaom-av1 v3.6.0\n"
        ffm, enc = parse_versions(stderr, encoder="libaom-av1")
        assert ffm == "7.1"
        assert enc == "libaom-av1-3.6.0"

    def test_libaom_av1_aom_version_format(self) -> None:
        # Alternate format: "[libaom @ 0xabc] AOM version: 3.7.1"
        stderr = "ffmpeg version 7.1\n[libaom @ 0xabc123] AOM version: 3.7.1\n"
        ffm, enc = parse_versions(stderr, encoder="libaom-av1")
        assert ffm == "7.1"
        assert enc == "libaom-av1-3.7.1"

    def test_libaom_av1_no_banner_returns_stable_name(self) -> None:
        # Quiet build: no per-encoder banner — must return stable adapter name,
        # not "unknown" (regression guard for the pre-ADR-1077 bug).
        stderr = "ffmpeg version 7.1\n"
        _, enc = parse_versions(stderr, encoder="libaom-av1")
        assert enc == "libaom-av1"

    # --- libvvenc (ADR-1077) ---

    def test_libvvenc_with_version_banner(self) -> None:
        # FFmpeg libvvenc wrapper: "[libvvenc @ 0xabc] VVenC v1.14.0"
        stderr = "ffmpeg version 7.1\n[libvvenc @ 0xabc123] VVenC v1.14.0\n"
        ffm, enc = parse_versions(stderr, encoder="libvvenc")
        assert ffm == "7.1"
        assert enc == "libvvenc-1.14.0"

    def test_libvvenc_fraunhofer_prefix_variant(self) -> None:
        # Alternate format: "[libvvenc @ 0xabc] Fraunhofer VVC/H.266 Encoder VVenC v1.14.0"
        stderr = (
            "ffmpeg version 7.1\n"
            "[libvvenc @ 0xabc123] Fraunhofer VVC/H.266 Encoder VVenC v1.14.0\n"
        )
        ffm, enc = parse_versions(stderr, encoder="libvvenc")
        assert ffm == "7.1"
        assert enc == "libvvenc-1.14.0"

    def test_libvvenc_no_banner_returns_stable_name(self) -> None:
        # Quiet build: no per-encoder banner — must return stable adapter name,
        # not "unknown" (regression guard for the pre-ADR-1077 bug).
        stderr = "ffmpeg version 7.1\n"
        _, enc = parse_versions(stderr, encoder="libvvenc")
        assert enc == "libvvenc"


# ---------------------------------------------------------------------------
# bitrate_kbps
# ---------------------------------------------------------------------------


class TestBitrateKbps:
    def test_normal(self) -> None:
        assert bitrate_kbps(1_000_000, 10.0) == pytest.approx(800.0)

    def test_zero_duration_returns_zero(self) -> None:
        assert bitrate_kbps(1_000_000, 0.0) == 0.0

    def test_negative_duration_returns_zero(self) -> None:
        assert bitrate_kbps(1_000_000, -5.0) == 0.0

    def test_zero_bytes(self) -> None:
        assert bitrate_kbps(0, 10.0) == 0.0


# ---------------------------------------------------------------------------
# iter_grid
# ---------------------------------------------------------------------------


def test_iter_grid_cartesian_product() -> None:
    result = iter_grid(["fast", "slow"], [23, 28, 33])
    assert result == [
        ("fast", 23),
        ("fast", 28),
        ("fast", 33),
        ("slow", 23),
        ("slow", 28),
        ("slow", 33),
    ]


def test_iter_grid_empty_crfs() -> None:
    assert iter_grid(["medium"], []) == []


def test_iter_grid_empty_presets() -> None:
    assert iter_grid([], [23]) == []


# ---------------------------------------------------------------------------
# _tail
# ---------------------------------------------------------------------------


def test_tail_short_string_unchanged() -> None:
    assert _tail("hello", 10) == "hello"


def test_tail_truncates_long_string() -> None:
    text = "a" * 100
    result = _tail(text, 20)
    assert len(result) == 20
    assert result == "a" * 20


# ---------------------------------------------------------------------------
# run_encode — rc-failure and pass-1 size skip
# ---------------------------------------------------------------------------


class TestRunEncode:
    def test_failed_encode_sets_exit_status(self, tmp_path: Path) -> None:
        req = _make_req(tmp_path=tmp_path)

        def bad_runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            return type(
                "R",
                (),
                {
                    "returncode": 1,
                    "stderr": "ffmpeg version 6.0\nerror\n",
                    "stdout": "",
                },
            )()

        result = run_encode(req, runner=bad_runner)
        assert result.exit_status == 1
        assert result.encode_size_bytes == 0

    def test_pass1_does_not_stat_output(self, tmp_path: Path) -> None:
        stats = tmp_path / "stats"
        req = _make_req(encoder="libx264", pass_number=1, stats_path=stats, tmp_path=tmp_path)

        # Create a real output file so we can confirm it's NOT stat-ed
        output = tmp_path / "out.mp4"
        output.write_bytes(b"\x00" * 1024)

        def ok_runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            return type(
                "R",
                (),
                {"returncode": 0, "stderr": "ffmpeg version 6.0\n", "stdout": ""},
            )()

        result = run_encode(req, runner=ok_runner)
        # Pass-1 should not report bytes even if output exists
        assert result.exit_status == 0
        assert result.encode_size_bytes == 0

    def test_successful_encode_reports_size(self, tmp_path: Path) -> None:
        req = _make_req(output=tmp_path / "out.mp4", tmp_path=tmp_path)

        def ok_runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            # Touch the output file
            out = tmp_path / "out.mp4"
            out.write_bytes(b"\x00" * 512)
            return type(
                "R",
                (),
                {
                    "returncode": 0,
                    "stderr": "ffmpeg version 6.0\nx264 - core 164 r3107\n",
                    "stdout": "",
                },
            )()

        result = run_encode(req, runner=ok_runner)
        assert result.exit_status == 0
        assert result.encode_size_bytes == 512
        assert result.encoder_version == "libx264-164"

    def test_encoder_version_fallback_probe(self, tmp_path: Path) -> None:
        """When the main run returns 'unknown', the probe fallback fires."""
        # Clear probe cache before test so the probe actually runs.
        _PROBE_CACHE.clear()
        req = _make_req(encoder="libx264", output=tmp_path / "out.mp4", tmp_path=tmp_path)
        call_count = {"n": 0}

        def runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            call_count["n"] += 1
            if cmd[-1] == "-version":
                # ffmpeg -version path (note: single dash, not double)
                return type(
                    "R",
                    (),
                    {
                        "returncode": 0,
                        "stdout": "configuration: --enable-libx264",
                        "stderr": "",
                    },
                )()
            # Main encode — touch output
            out = tmp_path / "out.mp4"
            out.write_bytes(b"\x00" * 64)
            # Return stderr without x264 banner so version is "unknown"
            return type(
                "R",
                (),
                {"returncode": 0, "stderr": "ffmpeg version 6.0\n", "stdout": ""},
            )()

        result = run_encode(req, runner=runner)
        assert result.exit_status == 0
        # The probe ran and reported the "enabled" label
        assert result.encoder_version == "libx264-enabled"
        _PROBE_CACHE.clear()


# ---------------------------------------------------------------------------
# _probe_encoder_version_from_ffmpeg
# ---------------------------------------------------------------------------


class TestProbeEncoderVersion:
    def test_known_encoder_in_configure_line(self) -> None:
        _PROBE_CACHE.clear()

        def runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            return type(
                "R",
                (),
                {
                    "returncode": 0,
                    "stdout": "--enable-libsvtav1 --enable-libx264",
                    "stderr": "",
                },
            )()

        result = _probe_encoder_version_from_ffmpeg("ffmpeg", "libsvtav1", runner)
        assert result == "libsvtav1-enabled"
        _PROBE_CACHE.clear()

    def test_known_encoder_missing_from_configure_line(self) -> None:
        _PROBE_CACHE.clear()

        def runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            return type("R", (), {"returncode": 0, "stdout": "--enable-libvpx", "stderr": ""})()

        result = _probe_encoder_version_from_ffmpeg("ffmpeg", "libx264", runner)
        assert result == ""
        _PROBE_CACHE.clear()

    def test_unknown_encoder_returns_empty(self) -> None:
        result = _probe_encoder_version_from_ffmpeg(
            "ffmpeg", "some_hw_encoder", lambda *a, **kw: None
        )
        assert result == ""

    def test_os_error_returns_empty(self) -> None:
        _PROBE_CACHE.clear()

        def bad_runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            raise OSError("no such file")

        result = _probe_encoder_version_from_ffmpeg("ffmpeg", "libx264", bad_runner)
        assert result == ""
        _PROBE_CACHE.clear()

    def test_cache_hit_skips_runner(self) -> None:
        _PROBE_CACHE.clear()
        # Warm the cache
        _PROBE_CACHE[("ffmpeg", "libx264")] = "libx264-cached"

        called = {"n": 0}

        def runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            called["n"] += 1
            return type("R", (), {"returncode": 0, "stdout": "", "stderr": ""})()

        result = _probe_encoder_version_from_ffmpeg("ffmpeg", "libx264", runner)
        assert result == "libx264-cached"
        assert called["n"] == 0
        _PROBE_CACHE.clear()


# ---------------------------------------------------------------------------
# run_encode_with_stats
# ---------------------------------------------------------------------------


class TestRunEncodeWithStats:
    def test_capture_stats_false_delegates_to_run_encode(self, tmp_path: Path) -> None:
        req = _make_req(output=tmp_path / "out.mp4", tmp_path=tmp_path)

        def ok_runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            out = tmp_path / "out.mp4"
            out.write_bytes(b"\x00" * 256)
            return type(
                "R",
                (),
                {
                    "returncode": 0,
                    "stderr": "ffmpeg version 6.0\nx264 - core 164 r3107\n",
                    "stdout": "",
                },
            )()

        result = run_encode_with_stats(req, runner=ok_runner, capture_stats=False)
        assert result.exit_status == 0
        assert result.encoder_stats == ()  # no stats captured

    def test_capture_stats_with_empty_stats_file(self, tmp_path: Path) -> None:
        req = _make_req(output=tmp_path / "out.mp4", tmp_path=tmp_path)
        run_count = {"n": 0}

        def ok_runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            run_count["n"] += 1
            # Touch output on pass-2-ish calls (not null muxer)
            out_path = cmd[-1]
            if out_path not in ("-", "/dev/null") and not out_path.startswith("-"):
                try:
                    out = Path(out_path)
                    out.parent.mkdir(parents=True, exist_ok=True)
                    out.write_bytes(b"\x00" * 128)
                except OSError:
                    pass
            return type(
                "R",
                (),
                {"returncode": 0, "stderr": "ffmpeg version 6.0\n", "stdout": ""},
            )()

        result = run_encode_with_stats(
            req, runner=ok_runner, capture_stats=True, stats_dir=tmp_path
        )
        assert result.exit_status == 0
        # Stats tuple is empty because no stats file was produced by the fake runner
        assert result.encoder_stats == ()


# ---------------------------------------------------------------------------
# run_two_pass_encode
# ---------------------------------------------------------------------------


class TestRunTwoPassEncode:
    def test_unsupported_on_unsupported_raise(self, tmp_path: Path) -> None:
        # libsvtav1 has supports_two_pass=False
        req = _make_req(encoder="libsvtav1", output=tmp_path / "out.mp4", tmp_path=tmp_path)
        with pytest.raises(ValueError, match="does not support 2-pass"):
            run_two_pass_encode(req, on_unsupported="raise")

    def test_unsupported_on_unsupported_fallback_runs_single_pass(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        req = _make_req(encoder="libsvtav1", output=tmp_path / "out.mp4", tmp_path=tmp_path)

        def ok_runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            out = tmp_path / "out.mp4"
            out.write_bytes(b"\x00" * 128)
            return type(
                "R",
                (),
                {
                    "returncode": 0,
                    "stderr": "ffmpeg version 7.0\nSVT-AV1 ENCODER v2.1.0\n",
                    "stdout": "",
                },
            )()

        result = run_two_pass_encode(req, runner=ok_runner, on_unsupported="fallback")
        assert result.exit_status == 0

    def test_unsupported_unknown_on_unsupported_raises_value_error(self, tmp_path: Path) -> None:
        req = _make_req(encoder="libsvtav1", output=tmp_path / "out.mp4", tmp_path=tmp_path)
        with pytest.raises(ValueError, match="unknown on_unsupported"):
            run_two_pass_encode(req, on_unsupported="skip")

    def test_pass1_failure_short_circuits(self, tmp_path: Path) -> None:
        req = _make_req(encoder="libx264", output=tmp_path / "out.mp4", tmp_path=tmp_path)
        call_count = {"n": 0}

        def failing_pass1(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            call_count["n"] += 1
            # First call = pass-1, make it fail
            if call_count["n"] == 1:
                return type(
                    "R",
                    (),
                    {"returncode": 1, "stderr": "ffmpeg: pass 1 error\n", "stdout": ""},
                )()
            # Should never reach pass 2
            raise AssertionError("pass 2 should not be called after pass 1 failure")

        result = run_two_pass_encode(req, runner=failing_pass1, scratch_dir=tmp_path)
        assert result.exit_status == 1
        assert "[pass 1 failed]" in result.stderr_tail
        assert call_count["n"] == 1

    def test_combined_timing_and_size(self, tmp_path: Path) -> None:
        req = _make_req(encoder="libx264", output=tmp_path / "out.mp4", tmp_path=tmp_path)
        call_count = {"n": 0}

        def two_pass_runner(cmd: list[str], capture_output: bool, text: bool, check: bool) -> Any:
            call_count["n"] += 1
            if cmd[-1] == "-":
                # Pass 1 — null muxer
                return type(
                    "R",
                    (),
                    {"returncode": 0, "stderr": "ffmpeg version 6.0\n", "stdout": ""},
                )()
            # Pass 2 — touch output
            out = tmp_path / "out.mp4"
            out.write_bytes(b"\x00" * 1024)
            return type(
                "R",
                (),
                {
                    "returncode": 0,
                    "stderr": "ffmpeg version 6.0\nx264 - core 164 r3107\n",
                    "stdout": "",
                },
            )()

        result = run_two_pass_encode(req, runner=two_pass_runner, scratch_dir=tmp_path)
        assert result.exit_status == 0
        assert result.encode_size_bytes == 1024
        # Timing is sum of both passes (always > 0)
        assert result.encode_time_ms >= 0.0
        # ffmpeg_version comes from pass 2
        assert result.ffmpeg_version == "6.0"
        assert call_count["n"] == 2
