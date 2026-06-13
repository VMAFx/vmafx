# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for ADR-0498 follow-up #7.

Covers four sub-tasks implemented in a single PR:

(1) x264/x265/libvpx detection from configure summary —
    ``_VERSION_PROBE_PATTERNS`` now includes ``libx265`` and
    ``libvpx-vp9``; ``EncoderInfo`` dataclass with ``codec_detected``
    field; ``probe_encoder_info`` public helper.

(2) Two-pass stats-file regression — ``build_pass1_stats_command``
    had a duplicate ``fallback_duration`` assignment (dead code from
    the #1266 refactor); removed.

(3) Backend-specific encode-extract dispatch — ``_gpu_verify`` no
    longer has the ``_ = backend`` dead-assignment stub; the backend
    selected by ``score_backend.select_backend`` is forwarded to
    ``_build_production_sample_extractor`` so all 30 TPE probe-encode
    scores run on GPU when available.

(4) Codec-list parser — ``codec_adapters.parse_available_codecs``
    parses ``ffmpeg -hide_banner -encoders`` output into a frozenset
    of available codec names.
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

# ---------------------------------------------------------------------------
# Sub-task 1: EncoderInfo + x265/libvpx detection
# ---------------------------------------------------------------------------


def test_encoder_info_dataclass_fields() -> None:
    """EncoderInfo carries encoder, codec_detected, version_label fields."""
    from vmaftune.encode import EncoderInfo

    info = EncoderInfo(encoder="libx264", codec_detected=True, version_label="libx264-enabled")
    assert info.encoder == "libx264"
    assert info.codec_detected is True
    assert info.version_label == "libx264-enabled"


def test_probe_encoder_info_detected() -> None:
    """probe_encoder_info returns codec_detected=True when configure line matches."""
    from vmaftune.encode import _PROBE_CACHE, probe_encoder_info

    _PROBE_CACHE.clear()

    _configure_line = (
        "ffmpeg version n6.0\n"
        "configuration: --prefix=/usr --enable-libx264 --enable-libsvtav1 "
        "--enable-libx265 --enable-libvpx\n"
    )

    class _FakeCompleted:
        returncode = 0
        stdout = _configure_line
        stderr = ""

    def _runner(argv, **_kw):  # noqa: ARG001
        return _FakeCompleted()

    for enc in ("libx264", "libsvtav1", "libx265", "libvpx-vp9"):
        _PROBE_CACHE.clear()
        info = probe_encoder_info("ffmpeg", enc, _runner)
        assert info.codec_detected is True, f"expected codec_detected=True for {enc}"
        assert info.version_label == f"{enc}-enabled"


def test_probe_encoder_info_not_detected() -> None:
    """probe_encoder_info returns codec_detected=False when flag is absent."""
    from vmaftune.encode import _PROBE_CACHE, probe_encoder_info

    _PROBE_CACHE.clear()

    class _FakeCompleted:
        returncode = 0
        stdout = "ffmpeg version n6.0\nconfiguration: --prefix=/usr\n"
        stderr = ""

    def _runner(argv, **_kw):  # noqa: ARG001
        return _FakeCompleted()

    for enc in ("libx264", "libx265", "libvpx-vp9"):
        _PROBE_CACHE.clear()
        info = probe_encoder_info("ffmpeg", enc, _runner)
        assert info.codec_detected is False, f"expected codec_detected=False for {enc}"
        assert info.version_label == "unknown"


def test_probe_encoder_info_unknown_encoder() -> None:
    """probe_encoder_info returns codec_detected=False for unrecognised encoder."""
    from vmaftune.encode import _PROBE_CACHE, probe_encoder_info

    _PROBE_CACHE.clear()

    class _FakeCompleted:
        returncode = 0
        stdout = "ffmpeg version n6.0\nconfiguration: --enable-libx264\n"
        stderr = ""

    def _runner(argv, **_kw):  # noqa: ARG001
        return _FakeCompleted()

    info = probe_encoder_info("ffmpeg", "libfooav99", _runner)
    assert info.codec_detected is False
    assert info.version_label == "unknown"


def test_version_probe_patterns_cover_x265_and_libvpx() -> None:
    """_VERSION_PROBE_PATTERNS must include libx265 and libvpx-vp9."""
    from vmaftune.encode import _VERSION_PROBE_PATTERNS

    assert "libx265" in _VERSION_PROBE_PATTERNS
    assert "libvpx-vp9" in _VERSION_PROBE_PATTERNS
    # Existing entries must remain.
    assert "libx264" in _VERSION_PROBE_PATTERNS
    assert "libsvtav1" in _VERSION_PROBE_PATTERNS


# ---------------------------------------------------------------------------
# Sub-task 2: build_pass1_stats_command has no duplicate fallback_duration
# ---------------------------------------------------------------------------


def test_build_pass1_stats_command_no_duplicate_fallback_duration(tmp_path: Path) -> None:
    """Regression: duplicate fallback_duration removed from build_pass1_stats_command.

    Previously lines 502-504 and 506-508 of encode.py both assigned
    ``fallback_duration`` identically (dead first write from the #1266
    refactor). This test verifies the command is still correct after the
    cleanup — specifically that ``-t`` appears when ``duration_s > 0``
    and ``sample_clip_seconds == 0``.
    """
    from vmaftune.encode import EncodeRequest, build_pass1_stats_command

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x80" * 4096)
    req = EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
        duration_s=5.0,  # no sample_clip_seconds; fallback_duration should activate
    )
    prefix = tmp_path / "stats_prefix"
    cmd = build_pass1_stats_command(req, prefix, ffmpeg_bin="ffmpeg")
    assert "-t" in cmd, "expected -t duration clamp in pass-1 command"
    t_idx = cmd.index("-t")
    assert float(cmd[t_idx + 1]) == 5.0
    assert "-passlogfile" in cmd
    assert cmd[cmd.index("-passlogfile") + 1] == str(prefix)


# ---------------------------------------------------------------------------
# Sub-task 3: backend forwarded through sample extractor
# ---------------------------------------------------------------------------


def test_build_production_sample_extractor_accepts_backend() -> None:
    """_build_production_sample_extractor must accept and store the backend kwarg."""
    import inspect

    from vmaftune.fast import _build_production_sample_extractor

    sig = inspect.signature(_build_production_sample_extractor)
    assert "backend" in sig.parameters, (
        "_build_production_sample_extractor must have a 'backend' parameter "
        "(ADR-0498 follow-up #7 backend dispatch)"
    )


def test_build_prod_predictor_accepts_backend() -> None:
    """_build_prod_predictor must accept and forward the backend kwarg."""
    import inspect

    from vmaftune.fast import _build_prod_predictor

    sig = inspect.signature(_build_prod_predictor)
    assert "backend" in sig.parameters, (
        "_build_prod_predictor must have a 'backend' parameter "
        "(ADR-0498 follow-up #7 backend dispatch)"
    )


def test_gpu_verify_no_dead_backend_assignment() -> None:
    """_gpu_verify must not have the dead '_ = backend' stub.

    The stub was left over from when the backend was not yet forwarded
    to the encode runner. After ADR-0498 follow-up #7 the backend is
    forwarded directly.
    """
    import inspect

    from vmaftune.fast import _gpu_verify

    src = inspect.getsource(_gpu_verify)
    assert "_ = backend" not in src, (
        "_gpu_verify still contains the dead '_ = backend' stub; "
        "remove it (ADR-0498 follow-up #7)"
    )


def test_fast_sample_extractor_passes_backend_to_run_score(tmp_path: Path) -> None:
    """Backend kwarg must reach the run_score call inside the sample extractor."""
    import unittest.mock

    from vmaftune.fast import _build_production_sample_extractor

    calls: list[dict] = []

    def _fake_run_score(req, *, vmaf_bin="vmaf", backend=None, **kw):  # noqa: ARG001
        calls.append({"backend": backend})
        # Return a minimal fake ScoreResult.

        from vmaftune.score import ScoreResult

        return ScoreResult(
            request=req,
            vmaf_score=85.0,
            feature_means={},
            exit_status=0,
            stderr_tail="",
        )

    def _fake_run_encode(req, *, ffmpeg_bin="ffmpeg", runner=None):  # noqa: ARG001

        from vmaftune.encode import EncodeResult

        # Create a fake output file.
        req.output.parent.mkdir(parents=True, exist_ok=True)
        req.output.write_bytes(b"\x00" * 512)
        return EncodeResult(
            request=req,
            encode_size_bytes=512,
            encode_time_ms=100.0,
            encoder_version="libx264-164",
            ffmpeg_version="n6.0",
            exit_status=0,
            stderr_tail="",
        )

    def _fake_probe_geometry(src, cfg, runner):  # noqa: ARG001
        return 64, 64, 24.0

    src = tmp_path / "src.yuv"
    src.write_bytes(b"\x80" * (64 * 64 * 3 // 2 * 5 * 24))  # ~5 s

    with (
        unittest.mock.patch("vmaftune.score.run_score", _fake_run_score),
        unittest.mock.patch("vmaftune.encode.run_encode", _fake_run_encode),
        unittest.mock.patch(
            "vmaftune.predictor_features._probe_video_geometry", _fake_probe_geometry
        ),
    ):
        extractor = _build_production_sample_extractor(backend="cuda")
        # Call the extractor directly; it should forward backend="cuda" to run_score.
        try:
            extractor(src, 23, "libx264")
        except Exception:  # noqa: BLE001
            pass  # Only care that run_score was called with the right backend.

    assert calls, "run_score was never called"
    assert (
        calls[0]["backend"] == "cuda"
    ), f"expected backend='cuda' forwarded to run_score, got {calls[0]['backend']!r}"


# ---------------------------------------------------------------------------
# Sub-task 4: codec-list parser
# ---------------------------------------------------------------------------

_SAMPLE_FFMPEG_ENCODERS = """\
Encoders:
 V..... = Video
 A..... = Audio
 S..... = Subtitle
 .F.... = Frame-level multithreading
 ..S... = Slice-level multithreading
 ...X.. = Codec is experimental
 ....B. = Supports draw_horiz_band
 .....D = Supports direct rendering method 1
 ------
 V..... libx264              H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10
 V..... libx265              libx265 H.265 / HEVC
 V..... libvpx-vp9           libvpx VP9
 V..... libsvtav1            SVT-AV1(Scalable Video Technology for AV1)
 V..... h264_nvenc           NVIDIA NVENC H.264 encoder
 V..... libx264rgb           libx264 H.264 / AVC / MPEG-4 AVC (codec h264)
"""


def test_parse_available_codecs_basic() -> None:
    """parse_available_codecs returns known codecs present in the output."""
    from vmaftune.codec_adapters import parse_available_codecs

    result = parse_available_codecs(_SAMPLE_FFMPEG_ENCODERS)
    # These are known to the adapter registry.
    assert "libx264" in result
    assert "libx265" in result
    assert "libvpx-vp9" in result
    assert "libsvtav1" in result
    assert "h264_nvenc" in result


def test_parse_available_codecs_excludes_unknown() -> None:
    """parse_available_codecs with restrict_to_known=True excludes libx264rgb."""
    from vmaftune.codec_adapters import parse_available_codecs

    result = parse_available_codecs(_SAMPLE_FFMPEG_ENCODERS, restrict_to_known=True)
    # libx264rgb is not in the adapter registry.
    assert "libx264rgb" not in result


def test_parse_available_codecs_unrestricted() -> None:
    """parse_available_codecs with restrict_to_known=False includes all parsed names."""
    from vmaftune.codec_adapters import parse_available_codecs

    result = parse_available_codecs(_SAMPLE_FFMPEG_ENCODERS, restrict_to_known=False)
    assert "libx264rgb" in result


def test_parse_available_codecs_empty_input() -> None:
    """parse_available_codecs on empty / header-only input returns empty frozenset."""
    from vmaftune.codec_adapters import parse_available_codecs

    assert parse_available_codecs("") == frozenset()
    assert parse_available_codecs("Encoders:\n ------\n") == frozenset()


def test_parse_available_codecs_returns_frozenset() -> None:
    """Return type is frozenset (hashable, immutable)."""
    from vmaftune.codec_adapters import parse_available_codecs

    result = parse_available_codecs(_SAMPLE_FFMPEG_ENCODERS)
    assert isinstance(result, frozenset)


def test_parse_available_codecs_in_all_export() -> None:
    """parse_available_codecs must be in codec_adapters.__all__."""
    import vmaftune.codec_adapters as ca

    assert "parse_available_codecs" in ca.__all__
