# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the BBB end-to-end probe v8 bug cluster (ADR-0508).

The v8 probe surfaced one follow-up against the v6 V6-1 fix:

* **V8-A** — ``ladder --duration N`` against a libx264 corpus run still
  re-encoded the full source during the ffmpeg pass-1 stats sweep.
  ``build_ffmpeg_command`` honoured ``EncodeRequest.duration_s`` after
  V6-1, but ``build_pass1_stats_command`` (the path taken by
  :func:`vmaftune.encode.run_encode_with_stats` for codec adapters
  declaring ``supports_encoder_stats=True`` — libx264 in particular)
  was a sibling argv-builder that did NOT mirror the V6-1 fallback,
  so the pass-1 invocation skipped ``-t N`` and ran over the full
  source. Net effect: a ``--duration 5`` smoke run against a
  10-minute BBB source still burned ~10 min of wall time per cell on
  pass 1 alone before the V6-1-honouring pass 2 ran on the requested
  5-second window.

  The fix mirrors the V6-1 fallback in
  :func:`vmaftune.encode.build_pass1_stats_command`: when the caller
  bound ``req.duration_s > 0`` without opting into sample-clip mode,
  add ``-t req.duration_s`` on the input side just like
  :func:`build_ffmpeg_command` does.
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.encode import EncodeRequest, build_pass1_stats_command  # noqa: E402

# ---------------------------------------------------------------------------
# V8-A — pass-1 stats argv mirrors the V6-1 ``-t duration_s`` fallback
# ---------------------------------------------------------------------------


def test_v8_a_pass1_stats_container_source_duration_clips_encode(
    tmp_path: Path,
) -> None:
    """A container source with ``duration_s > 0`` clips the pass-1 stats sweep.

    Pins the V8-A fix. Without it the pass-1 stats command ran over
    the full source while pass 2 honoured the bound window — net
    result is the encode-cluster still scaling with source length
    instead of ``--duration``.
    """
    req = EncodeRequest(
        source=tmp_path / "src.mp4",
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=30.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
        source_is_container=True,
        duration_s=5.0,
    )
    cmd = build_pass1_stats_command(req, tmp_path / "stats_prefix")
    assert "-t" in cmd, f"pass-1 cmd lacks -t flag: {cmd!r}"
    t_idx = cmd.index("-t")
    assert cmd[t_idx + 1] == "5.0", f"pass-1 -t value not 5.0: {cmd!r}"
    # -t must precede -i (input-side clip), not be an output-side seek.
    i_idx = cmd.index("-i")
    assert t_idx < i_idx, f"pass-1 -t must precede -i (input-side): {cmd!r}"


def test_v8_a_pass1_stats_raw_source_duration_clips_encode(tmp_path: Path) -> None:
    """Raw YUV sources also honour ``duration_s`` as an input-side ``-t`` on pass 1."""
    req = EncodeRequest(
        source=tmp_path / "src.yuv",
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=30.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
        source_is_container=False,
        duration_s=7.5,
    )
    cmd = build_pass1_stats_command(req, tmp_path / "stats_prefix")
    assert "-t" in cmd, f"pass-1 cmd lacks -t flag: {cmd!r}"
    t_idx = cmd.index("-t")
    assert cmd[t_idx + 1] == "7.5", f"pass-1 -t value not 7.5: {cmd!r}"
    i_idx = cmd.index("-i")
    assert t_idx < i_idx, f"pass-1 -t must precede -i (input-side): {cmd!r}"


def test_v8_a_pass1_stats_sample_clip_takes_precedence_over_duration(
    tmp_path: Path,
) -> None:
    """``sample_clip_seconds`` keeps precedence over the ``duration_s`` fallback on pass 1.

    Matches the V6-1 precedence: when the caller opts into sample-clip
    mode the explicit ``-ss / -t sample_clip_seconds`` argv wins, and
    the ``duration_s`` fallback stays out of the argv so the two flags
    don't double up.
    """
    req = EncodeRequest(
        source=tmp_path / "src.mp4",
        width=1280,
        height=720,
        pix_fmt="yuv420p",
        framerate=30.0,
        encoder="libx264",
        preset="medium",
        crf=28,
        output=tmp_path / "out.mp4",
        source_is_container=True,
        sample_clip_seconds=3.0,
        sample_clip_start_s=1.0,
        duration_s=60.0,
    )
    cmd = build_pass1_stats_command(req, tmp_path / "stats_prefix")
    # Exactly one -t (the sample-clip one) and exactly one -ss.
    assert cmd.count("-t") == 1, f"expected single -t under sample-clip: {cmd!r}"
    assert cmd.count("-ss") == 1, f"expected single -ss under sample-clip: {cmd!r}"
    t_idx = cmd.index("-t")
    assert cmd[t_idx + 1] == "3.0", f"-t value should be sample_clip_seconds: {cmd!r}"


def test_v8_a_pass1_stats_no_duration_no_clip(tmp_path: Path) -> None:
    """The legacy code path (no clip, no duration) emits no ``-t`` on pass 1.

    Guard against regressions that always emit ``-t`` even with the
    default ``duration_s=0.0`` — the historic full-source behaviour
    must remain reachable for callers that didn't opt in to clipping.
    """
    req = EncodeRequest(
        source=tmp_path / "src.yuv",
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=30.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
        source_is_container=False,
        # duration_s defaults to 0.0, sample_clip_seconds defaults to 0.0
    )
    cmd = build_pass1_stats_command(req, tmp_path / "stats_prefix")
    assert "-t" not in cmd, f"expected no -t on default path: {cmd!r}"
