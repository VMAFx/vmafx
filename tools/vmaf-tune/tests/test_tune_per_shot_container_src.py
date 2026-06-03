# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Fix A smoke tests — tune-per-shot accepts container sources directly.

ADR-0542: when --src is a container (mp4/mkv/mov/etc.) the CLI now
auto-probes width, height, framerate, and total-frames via ffprobe
so the operator is not required to pre-extract a raw YUV or supply
geometry flags manually.

These tests mock ffprobe (via a fake probe_source) and the
vmaf-perShot binary so no real media files are needed.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import cli  # noqa: E402
from vmaftune.report import SourceInfo  # noqa: E402

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _fake_probe(path: Path) -> SourceInfo:
    """Synthetic ffprobe result for a 1920x1080/60fps/634s container."""
    return SourceInfo(
        path=str(path),
        width=1920,
        height=1080,
        fps=60.0,
        duration_s=634.0,
        frame_count=38040,
        codec="h264",
        size_bytes=1_000_000,
    )


def _fake_probe_zero(path: Path) -> SourceInfo:
    """Probe that returns zero dimensions — simulates a failing probe."""
    return SourceInfo(
        path=str(path),
        width=0,
        height=0,
        fps=0.0,
        duration_s=0.0,
        frame_count=0,
        codec="unknown",
        size_bytes=0,
    )


def _make_per_shot_args(src: Path, **overrides) -> SimpleNamespace:
    """Build a minimal argparse-like namespace for tune-per-shot."""
    base = {
        "src": src,
        "width": None,
        "height": None,
        "pix_fmt": "yuv420p",
        "framerate": None,
        "target_vmaf": 92.0,
        "encoder": "libx264",
        "bitdepth": 8,
        "total_frames": 0,
        "scene_threshold": None,
        "max_shot_duration": 2.0,
        "per_shot_bin": "vmaf-perShot",
        "ffmpeg_bin": "ffmpeg",
        "vmaf_bin": "vmaf",
        "preset": None,
        "crf_min": None,
        "crf_max": None,
        "max_iterations": 8,
        "vmaf_model": "vmaf_v0.6.1",
        "score_backend": "auto",
        "predicate_module": None,
        "output": Path("per_shot_encode.mp4"),
        "segment_dir": None,
        "plan_out": None,
        "script_out": None,
    }
    base.update(overrides)
    return SimpleNamespace(**base)


# ---------------------------------------------------------------------------
# Fix A: container source auto-probe
# ---------------------------------------------------------------------------


def test_per_shot_probes_width_height_from_container(monkeypatch, tmp_path):
    """Width and height are auto-filled from probe when --src is a container."""
    src = tmp_path / "clip.mp4"
    src.write_bytes(b"fake mp4 content")

    # Monkeypatch probe_source so it returns known geometry.
    monkeypatch.setattr("vmaftune.cli.sys", sys)
    captured_args: list = []

    def _fake_detect_shots(path, *, width, height, **kwargs):
        captured_args.append((width, height))
        from vmaftune.per_shot import Shot

        return [Shot(start_frame=0, end_frame=120)]

    monkeypatch.setattr("vmaftune.cli.detect_shots", _fake_detect_shots)

    args = _make_per_shot_args(src, plan_out=tmp_path / "plan.json")

    # Inject the fake probe.
    import vmaftune.report as _report  # noqa: PLC0415

    monkeypatch.setattr(_report, "probe_source", _fake_probe)

    # Inject a trivial predicate that never encodes.
    def _fake_pred(shot, target_vmaf, encoder):
        return (23, target_vmaf)

    monkeypatch.setattr("vmaftune.cli._load_per_shot_predicate", lambda _spec: _fake_pred)
    # Force the predicate-module path so we skip real bisect.
    args.predicate_module = "fake:pred"

    rc = cli._run_tune_per_shot(args)

    assert rc == 0, "expected exit 0"
    assert captured_args, "detect_shots was not called"
    w, h = captured_args[0]
    assert w == 1920, f"expected probed width 1920, got {w}"
    assert h == 1080, f"expected probed height 1080, got {h}"
    assert args.framerate == 60.0, f"expected probed framerate 60.0, got {args.framerate}"


def test_per_shot_uses_explicit_width_height_when_provided(monkeypatch, tmp_path):
    """Explicit --width/--height override the probe even for a container source."""
    src = tmp_path / "clip.mp4"
    src.write_bytes(b"fake mp4 content")

    captured_args: list = []

    def _fake_detect_shots(path, *, width, height, **kwargs):
        captured_args.append((width, height))
        from vmaftune.per_shot import Shot

        return [Shot(start_frame=0, end_frame=60)]

    monkeypatch.setattr("vmaftune.cli.detect_shots", _fake_detect_shots)

    # Explicit override: 1280x720 (different from the probed 1920x1080).
    args = _make_per_shot_args(
        src, width=1280, height=720, framerate=30.0, plan_out=tmp_path / "plan.json"
    )

    def _fake_pred(shot, target_vmaf, encoder):
        return (23, target_vmaf)

    monkeypatch.setattr("vmaftune.cli._load_per_shot_predicate", lambda _spec: _fake_pred)
    args.predicate_module = "fake:pred"

    # probe_source should NOT be called when geometry is fully explicit.
    probe_called = []

    import vmaftune.report as _report  # noqa: PLC0415

    def _tracking_probe(path):
        probe_called.append(path)
        return _fake_probe(path)

    monkeypatch.setattr(_report, "probe_source", _tracking_probe)

    rc = cli._run_tune_per_shot(args)

    assert rc == 0
    w, h = captured_args[0]
    assert w == 1280
    assert h == 720
    # probe should not have been called for explicit geometry
    assert not probe_called, "probe_source was called even though geometry was explicit"


def test_per_shot_errors_on_raw_yuv_with_missing_geometry(tmp_path, capsys):
    """raw YUV source with no --width/--height must exit 2."""
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 100)

    args = _make_per_shot_args(src)  # width=None, height=None by default

    rc = cli._run_tune_per_shot(args)
    assert rc == 2
    captured = capsys.readouterr()
    assert "--width" in captured.err or "width" in captured.err


def test_per_shot_plan_has_shots_key_for_container_source(monkeypatch, tmp_path):
    """plan.json emitted for a container source contains a 'shots' list."""
    src = tmp_path / "clip.mkv"
    src.write_bytes(b"fake mkv content")

    from vmaftune.per_shot import Shot  # noqa: PLC0415

    def _fake_detect_shots(path, **kwargs):
        return [Shot(0, 30), Shot(30, 60), Shot(60, 120)]

    monkeypatch.setattr("vmaftune.cli.detect_shots", _fake_detect_shots)

    import vmaftune.report as _report  # noqa: PLC0415

    monkeypatch.setattr(_report, "probe_source", _fake_probe)

    plan_out = tmp_path / "plan.json"
    args = _make_per_shot_args(src, plan_out=plan_out)

    def _fake_pred(shot, target_vmaf, encoder):
        return (23, target_vmaf)

    monkeypatch.setattr("vmaftune.cli._load_per_shot_predicate", lambda _spec: _fake_pred)
    args.predicate_module = "fake:pred"

    rc = cli._run_tune_per_shot(args)
    assert rc == 0
    plan = json.loads(plan_out.read_text())
    assert "shots" in plan
    assert len(plan["shots"]) == 3
