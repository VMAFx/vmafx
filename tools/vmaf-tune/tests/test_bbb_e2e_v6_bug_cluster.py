# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the BBB end-to-end probe v6 bug cluster (ADR-0506).

The v6 probe surfaced three follow-ups against the v5 fixes:

* **V6-1** — ``ladder --duration N`` was metadata-only. The encode
  pipe never saw ``-t N`` so a 10-second smoke run against a 9-minute
  container source re-encoded the full 9 min at every CRF in the
  sweep. The fix threads ``CorpusJob.duration_s`` into the
  ``EncodeRequest`` and the ffmpeg argv adds ``-t duration_s`` on the
  input side whenever the caller didn't opt into sample-clip mode.
* **V6-2** — A cross-resolution ladder against a raw-YUV source failed
  on every rung whose target differed from the source dims. The v4
  scale-on-reference-decode path called ffmpeg without raw-input
  flags (``-f rawvideo -pix_fmt -s -r`` before ``-i``), so the
  rawvideo demuxer refused the input and the rung yielded zero
  scorable encodes. The fix passes the source geometry through to
  ``_decode_source_to_yuv`` and synthesises the demuxer flags when
  the source is raw.
* **V6-3** — ``vmaf-tune ladder`` returned exit code 0 even when the
  sampler raised ``RuntimeError`` ("default sampler produced no
  scorable encodes"), defeating CI / shell-script error handling.
  The fix wraps ``build_and_emit`` in try/except and returns 2 on
  any operational failure (matching the other vmaf-tune subcommands).
"""

from __future__ import annotations

import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.cli import main as cli_main  # noqa: E402
from vmaftune.corpus import (  # noqa: E402
    CorpusJob,
    CorpusOptions,
    _maybe_decode_reference,
    iter_rows,
)
from vmaftune.encode import EncodeRequest, build_ffmpeg_command  # noqa: E402


@dataclass
class _FakeCompleted:
    returncode: int = 0
    stdout: str = ""
    stderr: str = ""


# ---------------------------------------------------------------------------
# V6-1 — --duration clips the ffmpeg encode pipe
# ---------------------------------------------------------------------------


def test_v6_1_container_source_duration_clips_encode(tmp_path: Path) -> None:
    """A container source with ``duration_s > 0`` appends ``-t duration_s``.

    Pins the V6-1 fix: when the caller bound ``--duration 10`` on a
    container source but did NOT opt into sample-clip mode, the
    encode argv must still include ``-t 10`` on the input side so
    ffmpeg encodes only the first 10 seconds. Without this guard the
    v6 probe re-encoded a 9-minute BBB container at every CRF.
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
        duration_s=10.0,
    )
    cmd = build_ffmpeg_command(req)
    # -t must be input-side (before -i), so its index must precede -i.
    assert "-t" in cmd, cmd
    assert "-i" in cmd, cmd
    assert cmd.index("-t") < cmd.index("-i"), cmd
    assert float(cmd[cmd.index("-t") + 1]) == 10.0


def test_v6_1_raw_source_duration_clips_encode(tmp_path: Path) -> None:
    """Raw YUV sources also honour ``duration_s`` as an input-side ``-t``."""
    req = EncodeRequest(
        source=tmp_path / "src.yuv",
        width=854,
        height=480,
        pix_fmt="yuv420p",
        framerate=30.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
        source_is_container=False,
        duration_s=5.0,
    )
    cmd = build_ffmpeg_command(req)
    assert "-t" in cmd, cmd
    assert cmd.index("-t") < cmd.index("-i"), cmd
    assert float(cmd[cmd.index("-t") + 1]) == 5.0


def test_v6_1_sample_clip_takes_precedence_over_duration(tmp_path: Path) -> None:
    """When ``sample_clip_seconds`` is set it overrides ``duration_s``.

    Sample-clip mode (ADR-0297) is a centred N-second window inside
    ``duration_s``; both flags coexist by design — the encode driver
    must use the sample-clip values when both are positive.
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
        duration_s=60.0,
        sample_clip_seconds=10.0,
        sample_clip_start_s=25.0,
    )
    cmd = build_ffmpeg_command(req)
    # Sample-clip wins: -ss 25 -t 10, NOT -t 60.
    assert "-ss" in cmd, cmd
    assert float(cmd[cmd.index("-t") + 1]) == 10.0
    assert float(cmd[cmd.index("-ss") + 1]) == 25.0


def test_v6_1_iter_rows_threads_duration_into_encode_request(
    tmp_path: Path, monkeypatch: Any
) -> None:
    """``iter_rows`` plumbs ``CorpusJob.duration_s`` into ``EncodeRequest``.

    Pins the corpus-side wiring: the encoder runner sees a request
    whose ``duration_s`` matches the job, so the encode argv is
    bounded even when the CLI doesn't opt into sample-clip mode.
    """
    import vmaftune.corpus as corpus_mod

    src = tmp_path / "src.mp4"
    src.write_bytes(b"\x00")
    job = CorpusJob(
        source=src,
        width=1280,
        height=720,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=7.0,
        cells=(("medium", 28),),
    )
    opts = CorpusOptions(
        encoder="libx264",
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "enc",
        src_sha256=False,
    )

    seen_durations: list[float] = []

    def _enc_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        # Pin: -t is present in the encode argv before -i.
        if "-t" in argv and "-i" in argv:
            if argv.index("-t") < argv.index("-i"):
                seen_durations.append(float(argv[argv.index("-t") + 1]))
        out = Path(argv[-1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.0\n")

    def _score_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        out = Path(argv[argv.index("--output") + 1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text('{"pooled_metrics": {"vmaf": {"mean": 92.0}}}\n')
        return _FakeCompleted(returncode=0, stderr="VMAF version 3.0.0-test\n")

    def _fake_sp_run(argv: list[Any], **_kw: Any) -> _FakeCompleted:
        # ffmpeg ref-decode + distorted-decode runners go via _sp.run;
        # write the sidecar so the score step finds something.
        out = Path(argv[-1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0)

    monkeypatch.setattr(subprocess, "run", _fake_sp_run)
    monkeypatch.setattr(corpus_mod, "_sha256_file", lambda *_a, **_kw: "")

    rows = list(iter_rows(job, opts, encode_runner=_enc_runner, score_runner=_score_runner))
    assert rows
    assert seen_durations, "encode argv never carried -t"
    assert seen_durations[0] == 7.0


# ---------------------------------------------------------------------------
# V6-2 — raw-YUV cross-res reference decode emits input-side raw flags
# ---------------------------------------------------------------------------


def test_v6_2_raw_yuv_cross_res_decode_emits_input_flags(tmp_path: Path) -> None:
    """Cross-res reference decode on a raw source includes ``-f rawvideo -s WxH``."""
    captured: list[list[str]] = []

    def _runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        captured.append(list(argv))
        out_path = Path(argv[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0)

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * (1280 * 720 * 3 // 2))
    decoded, rc = _maybe_decode_reference(
        src,
        encode_dir=tmp_path / "enc",
        pix_fmt="yuv420p",
        duration_s=2.0,
        ffmpeg_bin="ffmpeg",
        runner=_runner,
        target_width=854,
        target_height=480,
        source_width=1280,
        source_height=720,
        source_framerate=30.0,
    )
    assert rc == 0
    assert decoded.exists()
    assert captured, "ffmpeg decode never invoked"
    argv = captured[0]
    # Demuxer-side raw flags must precede -i so ffmpeg can parse the source.
    i_idx = argv.index("-i")
    assert "-f" in argv[:i_idx], argv
    f_idx = argv.index("-f")
    assert f_idx < i_idx
    assert argv[f_idx + 1] == "rawvideo"
    assert "-s" in argv[:i_idx], argv
    s_idx = argv.index("-s")
    assert s_idx < i_idx
    assert argv[s_idx + 1] == "1280x720"
    # Output-side scale to the rung target.
    assert "-vf" in argv
    assert argv[argv.index("-vf") + 1] == "scale=854:480"


def test_v6_2_container_cross_res_keeps_auto_detect(tmp_path: Path) -> None:
    """Container sources skip the raw-input flags (back-compat)."""
    captured: list[list[str]] = []

    def _runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        captured.append(list(argv))
        out_path = Path(argv[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0)

    src = tmp_path / "ref.mp4"
    src.write_bytes(b"\x00")
    _decoded, rc = _maybe_decode_reference(
        src,
        encode_dir=tmp_path / "enc",
        pix_fmt="yuv420p",
        duration_s=2.0,
        ffmpeg_bin="ffmpeg",
        runner=_runner,
        target_width=854,
        target_height=480,
        source_width=1280,
        source_height=720,
        source_framerate=30.0,
    )
    assert rc == 0
    assert captured
    argv = captured[0]
    i_idx = argv.index("-i")
    # No -f rawvideo / -s before -i; ffmpeg auto-detects the container.
    assert "-f" not in argv[:i_idx], argv
    assert "-s" not in argv[:i_idx], argv


def test_v6_2_raw_yuv_same_res_skips_decode(tmp_path: Path) -> None:
    """When the rung target matches the raw source the decode is a no-op."""
    captured: list[list[str]] = []

    def _runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        captured.append(list(argv))
        return _FakeCompleted(returncode=0)

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 1024)
    decoded, rc = _maybe_decode_reference(
        src,
        encode_dir=tmp_path / "enc",
        pix_fmt="yuv420p",
        duration_s=2.0,
        ffmpeg_bin="ffmpeg",
        runner=_runner,
        source_width=854,
        source_height=480,
        source_framerate=30.0,
    )
    assert rc == 0
    assert decoded == src
    assert not captured


# ---------------------------------------------------------------------------
# V6-3 — ladder CLI returns RC != 0 on RuntimeError
# ---------------------------------------------------------------------------


def test_v6_3_ladder_runtime_error_returns_nonzero(tmp_path: Path, monkeypatch: Any) -> None:
    """When ``build_and_emit`` raises, the CLI exits with RC=2.

    Pins the V6-3 fix: a sampler that produces no scorable encodes
    raises ``RuntimeError``. The CLI must catch that and return 2;
    the historic path let the traceback escape and the wrapper still
    returned 0 to the shell, defeating CI gates.
    """
    import vmaftune.cli as cli_mod

    def _explode(*_a: Any, **_kw: Any) -> str:
        raise RuntimeError("default sampler produced no scorable encodes for /tmp/none.yuv")

    monkeypatch.setattr(cli_mod, "build_and_emit", _explode, raising=False)
    # ``_run_ladder`` does ``from .ladder import build_and_emit`` at
    # the top so patch the module-level import seam in the ladder
    # module as well.
    import vmaftune.ladder as ladder_mod

    monkeypatch.setattr(ladder_mod, "build_and_emit", _explode, raising=False)

    rc = cli_main(
        [
            "ladder",
            "--src",
            str(tmp_path / "nonexistent.yuv"),
            "--target-vmafs",
            "88,92",
            "--resolutions",
            "1280x720",
            "--framerate",
            "30",
            "--duration",
            "1",
            "--crf-sweep",
            "23",
        ]
    )
    assert rc == 2


def test_v6_3_ladder_nonexistent_src_returns_nonzero(tmp_path: Path) -> None:
    """End-to-end pin via subprocess: nonexistent --src yields non-zero RC.

    Pins the V6-3 contract through the actual ``main()`` dispatch
    rather than a unit-level patch: when the sampler can't produce
    any scorable encodes against a missing source, the process exits
    with a non-zero status.
    """
    proc = subprocess.run(
        [
            sys.executable,
            "-c",
            "from vmaftune.cli import main; raise SystemExit(main())",
            "ladder",
            "--src",
            str(tmp_path / "this_file_does_not_exist.yuv"),
            "--target-vmafs",
            "88",
            "--resolutions",
            "1280x720",
            "--framerate",
            "30",
            "--duration",
            "1",
            "--crf-sweep",
            "23",
            "--src-width",
            "1280",
            "--src-height",
            "720",
        ],
        capture_output=True,
        text=True,
        env={
            "PATH": "/usr/bin:/bin",
            "PYTHONPATH": str(Path(__file__).resolve().parents[1] / "src"),
        },
    )
    assert proc.returncode != 0, f"expected non-zero RC, got 0; stderr={proc.stderr!r}"
