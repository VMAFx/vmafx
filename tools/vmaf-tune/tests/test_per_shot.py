# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Phase D smoke tests — mocks vmaf-perShot + Phase B predicate.

Covers:
* Three-shot detection routes through complexity-aware predicate and
  yields three different CRFs.
* Single-shot fallback fires when ``vmaf-perShot`` is unavailable.
* :func:`merge_shots` emits a well-formed FFmpeg per-segment + concat
  command pair.
* CLI entrypoint extracts shots and binds the real Phase-B predicate
  seam, with subprocess + bisect monkeypatched.
"""

from __future__ import annotations

import io
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

# Make src/ importable without an editable install (Phase A pattern).
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

import itertools

from vmaftune import cli
from vmaftune.per_shot import (
    EncodingPlan,
    Shot,
    ShotRecommendation,
    detect_shots,
    merge_shots,
    parse_per_shot_csv,
    plan_to_shell_script,
    split_long_shots,
    tune_per_shot,
    write_concat_listing,
)


class _FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


# --------------------------------------------------------------------------- #
# Shot dataclass                                                              #
# --------------------------------------------------------------------------- #


def test_shot_rejects_invalid_range():
    with pytest.raises(ValueError):
        Shot(start_frame=10, end_frame=5)
    with pytest.raises(ValueError):
        Shot(start_frame=0, end_frame=0)
    with pytest.raises(ValueError):
        Shot(start_frame=-1, end_frame=10)


def test_shot_length_is_half_open():
    assert Shot(start_frame=4, end_frame=48).length == 44


# --------------------------------------------------------------------------- #
# detect_shots                                                                #
# --------------------------------------------------------------------------- #


def test_detect_shots_falls_back_to_single_shot_when_binary_missing(monkeypatch, tmp_path):
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 16)
    monkeypatch.setattr("vmaftune.per_shot._which", lambda _b: None)

    shots = detect_shots(src, width=64, height=64, total_frames=120)
    assert shots == [Shot(start_frame=0, end_frame=120)]


def test_detect_shots_fallback_no_total_frames(monkeypatch, tmp_path):
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 16)
    monkeypatch.setattr("vmaftune.per_shot._which", lambda _b: None)

    shots = detect_shots(src, width=64, height=64)
    assert shots == [Shot(start_frame=0, end_frame=1)]


def test_detect_shots_parses_per_shot_json(tmp_path):
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 16)
    payload = json.dumps(
        {
            "shots": [
                {"start_frame": 0, "end_frame": 23},
                {"start_frame": 24, "end_frame": 71},
                {"start_frame": 72, "end_frame": 119},
            ]
        }
    )

    def fake_run(cmd, capture_output, text, check):
        # Sanity-check the CLI shape we built.
        assert cmd[0] == "vmaf-perShot"
        assert "--reference" in cmd
        assert "--format" in cmd and cmd[cmd.index("--format") + 1] == "json"
        # The new protocol writes JSON to the --output tmpfile, not stdout.
        # Find the path passed as --output and write the fixture JSON there.
        out_path = Path(cmd[cmd.index("--output") + 1])
        out_path.write_text(payload, encoding="utf-8")
        progress = f"vmaf-perShot: wrote 3 shot(s) to {out_path}\n"
        return _FakeCompleted(returncode=0, stdout=progress)

    shots = detect_shots(
        src,
        width=128,
        height=128,
        per_shot_bin="vmaf-perShot",
        runner=fake_run,
    )
    # Half-open conversion: end_frame in source schema is inclusive.
    assert shots == [
        Shot(0, 24),
        Shot(24, 72),
        Shot(72, 120),
    ]


def test_detect_shots_falls_back_on_runner_failure(tmp_path):
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 16)

    def failing_run(cmd, capture_output, text, check):
        return _FakeCompleted(returncode=1, stdout="", stderr="boom")

    shots = detect_shots(
        src,
        width=64,
        height=64,
        total_frames=240,
        per_shot_bin="vmaf-perShot",
        runner=failing_run,
    )
    assert shots == [Shot(0, 240)]


def test_parse_per_shot_csv_round_trip():
    csv_text = (
        "shot_id,start_frame,end_frame,frames,mean_complexity,mean_motion,predicted_crf\n"
        "0,0,3,4,0.000051,0.020046,25.48\n"
        "1,4,47,44,0.019353,0.016716,24.62\n"
    )
    shots = parse_per_shot_csv(csv_text)
    assert shots == [Shot(0, 4), Shot(4, 48)]


# --------------------------------------------------------------------------- #
# tune_per_shot                                                               #
# --------------------------------------------------------------------------- #


def test_tune_per_shot_three_shots_yields_three_distinct_crfs():
    """Mocked complexity-aware predicate — busier shots get lower CRF."""
    shots = [Shot(0, 24), Shot(24, 72), Shot(72, 144)]
    complexity = {
        (0, 24): (0.05, 92.5),  # quiet — relax
        (24, 72): (0.30, 92.0),  # mid
        (72, 144): (0.85, 91.7),  # busy
    }

    def predicate(shot, target_vmaf, encoder):
        c, predicted = complexity[(shot.start_frame, shot.end_frame)]
        # Linear blend: CRF rises as complexity falls.
        crf = round(20 + (1.0 - c) * 12)
        # Sanity: caller passed the target through.
        assert target_vmaf == 92.0
        assert encoder == "libx264"
        return (crf, predicted)

    recs = tune_per_shot(shots, target_vmaf=92.0, encoder="libx264", predicate=predicate)
    assert len(recs) == 3
    crfs = [r.crf for r in recs]
    assert len(set(crfs)) == 3, f"expected three distinct CRFs, got {crfs}"
    # Quiet shot -> highest CRF; busy shot -> lowest.
    assert crfs[0] > crfs[1] > crfs[2]


def test_tune_per_shot_clamps_to_codec_quality_range():
    """Predicate values outside the adapter's `quality_range` clamp."""
    from vmaftune.codec_adapters import get_adapter

    lo, hi = get_adapter("libx264").quality_range

    def predicate(shot, target_vmaf, encoder):
        return (lo - 10, 95.0)  # below libx264's lower clamp

    recs = tune_per_shot([Shot(0, 24)], target_vmaf=92.0, predicate=predicate)
    assert recs[0].crf == lo

    def predicate_high(shot, target_vmaf, encoder):
        return (hi + 50, 50.0)  # above upper clamp

    recs = tune_per_shot([Shot(0, 24)], target_vmaf=92.0, predicate=predicate_high)
    assert recs[0].crf == hi


def test_tune_per_shot_default_predicate_returns_codec_default():
    recs = tune_per_shot([Shot(0, 24)], target_vmaf=92.0)
    assert recs[0].crf == 23  # libx264 default


def test_tune_per_shot_rejects_empty_input():
    with pytest.raises(ValueError):
        tune_per_shot([], target_vmaf=92.0)


# --------------------------------------------------------------------------- #
# merge_shots                                                                 #
# --------------------------------------------------------------------------- #


def test_merge_shots_emits_well_formed_ffmpeg_plan(tmp_path):
    src = tmp_path / "in.mp4"
    out = tmp_path / "out.mp4"
    recs = (
        ShotRecommendation(Shot(0, 24), crf=22, predicted_vmaf=93.0),
        ShotRecommendation(Shot(24, 72), crf=26, predicted_vmaf=92.5),
        ShotRecommendation(Shot(72, 144), crf=30, predicted_vmaf=91.8),
    )
    plan = merge_shots(
        recs,
        source=src,
        output=out,
        framerate=24.0,
        encoder="libx264",
    )

    assert isinstance(plan, EncodingPlan)
    assert len(plan.segment_commands) == 3
    # Each segment command has the libx264 + crf wiring at the right index.
    for cmd, rec in zip(plan.segment_commands, recs):
        assert cmd[0] == "ffmpeg"
        assert "-c:v" in cmd and cmd[cmd.index("-c:v") + 1] == "libx264"
        assert "-crf" in cmd and cmd[cmd.index("-crf") + 1] == str(rec.crf)
        # -ss seek + -frames:v are present.
        assert "-ss" in cmd
        assert "-frames:v" in cmd
        assert cmd[cmd.index("-frames:v") + 1] == str(rec.shot.length)

    # Concat command shape.
    assert plan.concat_command[0] == "ffmpeg"
    assert "-f" in plan.concat_command
    assert plan.concat_command[plan.concat_command.index("-f") + 1] == "concat"
    assert plan.concat_command[-1] == str(out)

    # Concat listing has one line per shot.
    listing_lines = [ln for ln in plan.concat_listing.splitlines() if ln.strip()]
    assert len(listing_lines) == 3
    assert all(ln.startswith("file '") for ln in listing_lines)


def test_merge_shots_rejects_empty_recommendations(tmp_path):
    with pytest.raises(ValueError):
        merge_shots(
            (),
            source=tmp_path / "x",
            output=tmp_path / "y",
            framerate=24.0,
        )


def test_write_concat_listing_persists(tmp_path):
    plan = merge_shots(
        (ShotRecommendation(Shot(0, 24), crf=23, predicted_vmaf=92.0),),
        source=tmp_path / "in.mp4",
        output=tmp_path / "out.mp4",
        framerate=24.0,
    )
    listing = tmp_path / "concat.txt"
    written = write_concat_listing(plan, listing)
    assert written == listing
    assert listing.read_text().startswith("file '")


def test_plan_to_shell_script_round_trip(tmp_path):
    plan = merge_shots(
        (ShotRecommendation(Shot(0, 24), crf=23, predicted_vmaf=92.0),),
        source=tmp_path / "in.mp4",
        output=tmp_path / "out.mp4",
        framerate=24.0,
    )
    script = plan_to_shell_script(plan)
    assert script.startswith("#!/bin/sh")
    assert "ffmpeg" in script


# --------------------------------------------------------------------------- #
# CLI smoke                                                                   #
# --------------------------------------------------------------------------- #


def _shots_payload(*ranges: tuple[int, int]) -> str:
    """``vmaf-perShot`` JSON for the given ``(start_frame, end_frame)`` ranges."""
    return json.dumps({"shots": [{"start_frame": s, "end_frame": e} for s, e in ranges]})


def _assert_is_ffmpeg(cmd) -> None:
    """The non-``vmaf-perShot`` leg of tune-per-shot must be the ffmpeg extract."""
    assert cmd[0] == "ffmpeg"


def _stub_per_shot_run(payload: str, *, stdout: str, on_ffmpeg=None):
    """``subprocess.run`` stand-in covering both legs of ``tune-per-shot``.

    The ``vmaf-perShot`` leg writes `payload` to the ``--output`` tmpfile
    (the current protocol: JSON to a file, progress on stdout). Every
    other argv is the ffmpeg segment extraction, which just materialises
    the raw YUV the next stage reads. `on_ffmpeg`, when given, inspects
    that argv before the file is written.
    """

    def _run(cmd, capture_output, text, check):
        if cmd[0] == "vmaf-perShot":
            out_path = Path(cmd[cmd.index("--output") + 1])
            out_path.write_text(payload, encoding="utf-8")
            return _FakeCompleted(returncode=0, stdout=stdout)
        if on_ffmpeg is not None:
            on_ffmpeg(cmd)
        out_yuv = Path(cmd[-1])
        out_yuv.write_bytes(b"\x00" * 16)
        return _FakeCompleted(returncode=0)

    return _run


def _stub_bisect_and_backend(monkeypatch, fake_bisect) -> None:
    """Patch the bisect predicate plus the ADR-0613 ``select_backend`` precheck.

    ``_run_tune_per_shot`` calls ``select_backend()`` before any work;
    left unpatched it invokes the real vmaf binary.
    """
    monkeypatch.setattr("vmaftune.cli.bisect_target_vmaf", fake_bisect)
    monkeypatch.setattr("vmaftune.cli.select_backend", lambda prefer, vmaf_bin: "cpu")


def _flag_argv(*pairs: tuple[str, str]) -> list[str]:
    """Flatten ``(flag, value)`` pairs into the flat token list argparse wants.

    Callers pass one tuple per CLI flag, so the formatter lays the argv out
    one flag-and-its-value per line — the way the command is read on a
    terminal — while ``main()`` still receives the flat ``["--width", "1920",
    ...]`` sequence.
    """
    return [token for pair in pairs for token in pair]


def _tune_per_shot_argv(src: Path, output: str, *extra: str) -> list[str]:
    """``tune-per-shot`` argv: 1080p24 source, target VMAF 92, CRF 18-30."""
    return [
        "tune-per-shot",
        *_flag_argv(
            ("--src", str(src)),
            ("--width", "1920"),
            ("--height", "1080"),
            ("--framerate", "24"),
            ("--target-vmaf", "92"),
            ("--encoder", "libx264"),
            ("--crf-min", "18"),
            ("--crf-max", "30"),
            ("--max-iterations", "4"),
            ("--output", output),
        ),
        *extra,
    ]


def _make_readonly_dir(tmp_path: Path, name: str) -> Path:
    """Create a read-only directory under `tmp_path`.

    Used as CWD so any relative write (e.g. ``Path("segments").mkdir()``)
    fails with ``PermissionError`` — the condition ADR-0530 / ADR-0532
    require the command to survive.
    """
    import stat

    d = tmp_path / name
    d.mkdir()
    d.chmod(stat.S_IRUSR | stat.S_IXUSR | stat.S_IRGRP | stat.S_IXGRP)
    return d


def _restore_writable(d: Path) -> None:
    """Give the directory back its owner write bit so pytest can clean up."""
    import stat

    d.chmod(stat.S_IRWXU)


def _capture_stderr(monkeypatch) -> io.StringIO:
    """Redirect ``sys.stderr`` into a buffer and return it."""
    buf = io.StringIO()
    monkeypatch.setattr(sys, "stderr", buf)
    return buf


def test_cli_tune_per_shot_binds_bisect_predicate(tmp_path, monkeypatch):
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 16)
    plan_out = tmp_path / "plan.json"
    out = tmp_path / "out.mp4"

    # Pretend the binary is available + intercept the subprocess call.
    monkeypatch.setattr("vmaftune.per_shot._which", lambda _b: "/fake/vmaf-perShot")

    extracted: list[Path] = []

    def _on_ffmpeg(cmd) -> None:
        _assert_is_ffmpeg(cmd)
        assert "-f" in cmd and "rawvideo" in cmd
        extracted.append(Path(cmd[-1]))

    monkeypatch.setattr(
        "vmaftune.per_shot.subprocess.run",
        _stub_per_shot_run(
            _shots_payload((0, 23), (24, 71)),
            stdout="vmaf-perShot: wrote 2 shot(s)\n",
            on_ffmpeg=_on_ffmpeg,
        ),
    )

    calls: list[tuple[Path, str, float]] = []

    def fake_bisect(src, codec, target_vmaf, **kwargs):
        calls.append((Path(src), codec, target_vmaf))
        crf = 21 + len(calls)
        return SimpleNamespace(
            ok=True,
            best_crf=crf,
            measured_vmaf=target_vmaf + len(calls) / 10.0,
            bitrate_kbps=1000.0 * len(calls),
            error="",
        )

    _stub_bisect_and_backend(monkeypatch, fake_bisect)

    rc = cli.main(_tune_per_shot_argv(src, str(out), "--plan-out", str(plan_out)))
    assert rc == 0
    assert plan_out.exists()
    doc = json.loads(plan_out.read_text())
    assert doc["encoder"] == "libx264"
    assert doc["predicate"] == "bisect"
    assert len(doc["shots"]) == 2
    assert [s["crf"] for s in doc["shots"]] == [22, 23]
    assert [s["predicted_vmaf"] for s in doc["shots"]] == [92.1, 92.2]
    assert len(calls) == 2
    assert calls[0][1:] == ("libx264", 92.0)
    assert len(extracted) == 2
    # Concat listing was written next to the segments.
    listing = (out.parent / "segments" / "concat.txt").read_text()
    assert listing.count("file '") == 2


# --------------------------------------------------------------------------- #
# ADR-0532 — tune-per-shot tolerates read-only CWD                            #
# ADR-0530 — tune-per-shot tolerates read-only CWD                            #
# --------------------------------------------------------------------------- #


def test_cli_tune_per_shot_readonly_cwd_returns_zero(tmp_path, monkeypatch):
    """tune-per-shot exits 0 even when CWD is read-only (ADR-0530, ADR-0532).

    The plan JSON is the primary deliverable.  When the segments directory
    cannot be created (e.g. a bind-mounted read-only container workspace), a
    WARN message is emitted to stderr and the command still returns 0.
    """
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 16)
    plan_out = tmp_path / "plan.json"
    out = tmp_path / "out.mp4"

    # The default --output resolves relative to CWD, so segments/ would land
    # in this read-only directory without the ADR-0530 fix.
    ro_dir = _make_readonly_dir(tmp_path, "ro_workspace")

    monkeypatch.setattr("vmaftune.per_shot._which", lambda _b: "/fake/vmaf-perShot")
    monkeypatch.setattr(
        "vmaftune.per_shot.subprocess.run",
        _stub_per_shot_run(
            _shots_payload((0, 23), (24, 47)),
            stdout="wrote 2 shot(s)",
            on_ffmpeg=_assert_is_ffmpeg,
        ),
    )

    def fake_bisect(src_path, codec, target_vmaf, **kwargs):
        return SimpleNamespace(
            ok=True,
            best_crf=23,
            measured_vmaf=target_vmaf,
            bitrate_kbps=2500.0,
            error="",
        )

    _stub_bisect_and_backend(monkeypatch, fake_bisect)

    monkeypatch.chdir(ro_dir)
    _capture_stderr(monkeypatch)

    rc = cli.main(_tune_per_shot_argv(src, str(out), "--plan-out", str(plan_out)))

    _restore_writable(ro_dir)

    assert rc == 0, f"expected exit 0, got {rc}"
    assert plan_out.exists(), "plan JSON must be written regardless of segments dir"
    doc = json.loads(plan_out.read_text())
    assert doc["encoder"] == "libx264"
    # The segments dir under plan_out.parent must have been created and contain
    # concat.txt (plan_out.parent is tmp_path, which is writable).
    listing = (plan_out.parent / "segments" / "concat.txt").read_text()
    assert listing.count("file '") == 2


def test_cli_tune_per_shot_ro_cwd_no_plan_out_warns(tmp_path, monkeypatch):
    """When neither --plan-out nor --segment-dir is given and CWD is read-only,
    a WARN is emitted to stderr and the command still returns 0 (ADR-0530).
    """
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 16)

    ro_dir = _make_readonly_dir(tmp_path, "ro_cwd")

    monkeypatch.setattr("vmaftune.per_shot._which", lambda _b: "/fake/vmaf-perShot")
    monkeypatch.setattr(
        "vmaftune.per_shot.subprocess.run",
        _stub_per_shot_run(_shots_payload((0, 23)), stdout="wrote 1 shot(s)"),
    )

    def fake_bisect(src_path, codec, target_vmaf, **kwargs):
        return SimpleNamespace(
            ok=True, best_crf=23, measured_vmaf=target_vmaf, bitrate_kbps=1800.0, error=""
        )

    _stub_bisect_and_backend(monkeypatch, fake_bisect)

    monkeypatch.chdir(ro_dir)
    stderr_capture = _capture_stderr(monkeypatch)

    # A relative --output resolves inside the read-only CWD, so the fallback
    # seg_dir (output.parent/segments == ro_cwd/segments) is non-writable.
    rc = cli.main(_tune_per_shot_argv(src, "per_shot_encode.mp4"))

    _restore_writable(ro_dir)

    assert rc == 0, f"expected exit 0, got {rc}"
    stderr_out = stderr_capture.getvalue()
    assert "WARN" in stderr_out and "not writable" in stderr_out


# --------------------------------------------------------------------------- #
# ADR-0513 — scene-threshold + uniform-window splitter                         #
# --------------------------------------------------------------------------- #


def test_split_long_shots_partitions_uniform_window():
    """A single 5 s shot at 60 fps with a 2 s window splits into 3 sub-shots
    of [120, 90, 90] frames (uniform within ±1 frame)."""
    shots = [Shot(start_frame=0, end_frame=300)]
    out = split_long_shots(shots, max_duration_sec=2.0, framerate=60.0)
    assert len(out) >= 3
    # Contiguity: end_i == start_{i+1}; full coverage of input range.
    assert out[0].start_frame == 0
    assert out[-1].end_frame == 300
    for a, b in itertools.pairwise(out):
        assert a.end_frame == b.start_frame
    # No partition longer than ceil(2.0 * 60) frames.
    assert max(s.length for s in out) <= 120


def test_split_long_shots_noop_when_disabled():
    shots = [Shot(start_frame=0, end_frame=300)]
    assert split_long_shots(shots, max_duration_sec=0.0, framerate=60.0) == shots
    # Non-finite framerate is a no-op too.
    assert split_long_shots(shots, max_duration_sec=2.0, framerate=float("nan")) == shots
    assert split_long_shots(shots, max_duration_sec=2.0, framerate=0.0) == shots


def test_split_long_shots_preserves_short_shots():
    """Shots already shorter than the window are passed through verbatim."""
    shots = [Shot(0, 30), Shot(30, 60), Shot(60, 90)]
    out = split_long_shots(shots, max_duration_sec=2.0, framerate=60.0)
    assert out == shots


def test_detect_shots_forwards_diff_threshold(tmp_path):
    """``--diff-threshold`` is added to the C-binary argv when supplied."""
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 16)
    payload = json.dumps({"shots": [{"start_frame": 0, "end_frame": 59}]})

    seen_cmd: list[list[str]] = []

    def fake_run(cmd, capture_output, text, check):
        seen_cmd.append(list(cmd))
        out_path = Path(cmd[cmd.index("--output") + 1])
        out_path.write_text(payload, encoding="utf-8")
        return _FakeCompleted(returncode=0, stdout="ok")

    detect_shots(
        src,
        width=128,
        height=128,
        per_shot_bin="vmaf-perShot",
        runner=fake_run,
        diff_threshold=4.5,
    )
    assert seen_cmd, "C binary was not invoked"
    cmd = seen_cmd[0]
    assert "--diff-threshold" in cmd
    idx = cmd.index("--diff-threshold")
    assert float(cmd[idx + 1]) == pytest.approx(4.5)


def test_detect_shots_splits_single_shot_via_max_duration(tmp_path):
    """5 s @ 60 fps with --max-shot-duration 2.0 yields >= 2 shots even
    when the C binary returns a single shot covering the whole clip.

    This is the deliverable-gate scenario from the BBB 5s case: the
    luma-delta heuristic returns one shot, and the uniform-window
    splitter ensures the per-shot tuner sees a non-degenerate timeline.
    """
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 16)
    payload = json.dumps({"shots": [{"start_frame": 0, "end_frame": 299}]})

    def fake_run(cmd, capture_output, text, check):
        out_path = Path(cmd[cmd.index("--output") + 1])
        out_path.write_text(payload, encoding="utf-8")
        return _FakeCompleted(returncode=0, stdout="ok")

    shots = detect_shots(
        src,
        width=3840,
        height=2160,
        per_shot_bin="vmaf-perShot",
        runner=fake_run,
        framerate=60.0,
        max_shot_duration_sec=2.0,
    )
    assert len(shots) >= 2
    # Contiguous coverage of [0, 300).
    assert shots[0].start_frame == 0
    assert shots[-1].end_frame == 300


# --------------------------------------------------------------------------- #
# ADR-0536 — per-shot predicate threads bitrate_kbps through bisect sidecar   #
# --------------------------------------------------------------------------- #


def test_cli_tune_per_shot_bitrate_kbps_propagates_from_bisect(tmp_path, monkeypatch):
    """Regression: plan JSON must carry real kbps numbers, not null.

    PR #1290 (ADR-0531) added ``ShotRecommendation.bitrate_kbps`` and the
    ``_shot_bitrate`` null-serialiser but ``_build_per_shot_bisect_predicate``
    discarded ``result.bitrate_kbps`` before returning ``(crf, vmaf)``.
    ADR-0536 fixes this via a bitrate sidecar dict populated by the predicate
    closure and consumed after :func:`tune_per_shot` completes.
    """
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 16)
    plan_out = tmp_path / "plan.json"
    out = tmp_path / "out.mp4"

    monkeypatch.setattr("vmaftune.per_shot._which", lambda _b: "/fake/vmaf-perShot")
    monkeypatch.setattr(
        "vmaftune.per_shot.subprocess.run",
        _stub_per_shot_run(
            _shots_payload((0, 47), (48, 95), (96, 143)),
            stdout="wrote 3 shot(s)",
            on_ffmpeg=_assert_is_ffmpeg,
        ),
    )

    # Fake bisect returns distinct bitrate_kbps per call so we can assert
    # they land in the right shot slots.
    call_index = [0]

    def fake_bisect(src_path, codec, target_vmaf, **kwargs):
        call_index[0] += 1
        return SimpleNamespace(
            ok=True,
            best_crf=20 + call_index[0],
            measured_vmaf=target_vmaf,
            bitrate_kbps=1000.0 * call_index[0],
            error="",
        )

    _stub_bisect_and_backend(monkeypatch, fake_bisect)

    rc = cli.main(_tune_per_shot_argv(src, str(out), "--plan-out", str(plan_out)))
    assert rc == 0
    doc = json.loads(plan_out.read_text())
    bitrates = [s["bitrate_kbps"] for s in doc["shots"]]
    # All three shots must carry a real kbps number — not null (ADR-0536).
    assert all(b is not None for b in bitrates), f"expected real kbps, got {bitrates}"
    assert bitrates == pytest.approx([1000.0, 2000.0, 3000.0])
