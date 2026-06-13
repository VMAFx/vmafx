# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the 2026-05-18 BBB end-to-end v3 bug cluster.

The v3 follow-up smoke run (after PR #1255 / ADR-0498 closed the v2
cluster) flagged one blocker plus one informational finding:

* **V3-B**: ``vmaf-tune ladder`` fails on container / Y4M sources
  because only the distorted leg was decoded to raw YUV before the
  vmaf CLI call. The reference (``.mp4`` / ``.y4m``) was passed in
  as-is and tripped ``raw_input_open``'s file-size guard. Sampler
  reported "produced no scorable encodes".
* **V3-C**: ``compare`` correctly marks unavailable encoders as
  ``ok=false`` with an "encoder unavailable" message (the bisect path
  already discriminates "Encoder not found" stderr from genuine encode
  failures, per ADR-0498 follow-up #6). Pinned here as a regression
  guard.

Pins one regression per finding per ADR-0499.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

import re  # noqa: E402

from vmaftune.corpus import (  # noqa: E402
    _VMAF_RAW_SUFFIXES,
    CorpusJob,
    CorpusOptions,
    _maybe_decode_reference,
    iter_rows,
)
from vmaftune.score import VMAF_RAW_SUFFIXES  # noqa: E402


@dataclass
class _FakeCompleted:
    returncode: int = 0
    stdout: str = ""
    stderr: str = ""


# ---------------------------------------------------------------------------
# Bug #V3-B — ladder/iter_rows decodes the reference for container sources
# ---------------------------------------------------------------------------


def test_maybe_decode_reference_decodes_container(tmp_path: Path) -> None:
    """A ``.mp4`` reference must be decoded to a raw YUV sidecar.

    Pins the new helper introduced by ADR-0499: when the source has a
    container extension, ``_maybe_decode_reference`` shells out to
    ffmpeg, emits a ``.ref.decoded.yuv`` sidecar under ``encode_dir``,
    and returns ``(decoded_path, 0)``.
    """
    captured: list[list[str]] = []

    def _runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        captured.append(list(argv))
        out_path = Path(argv[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0)

    src = tmp_path / "ref.mp4"
    src.write_bytes(b"\x00")
    encode_dir = tmp_path / "encodes"

    decoded, rc = _maybe_decode_reference(
        src,
        encode_dir=encode_dir,
        pix_fmt="yuv420p",
        duration_s=4.0,
        ffmpeg_bin="ffmpeg",
        runner=_runner,
    )

    assert rc == 0
    assert decoded != src, "container source must be replaced by decoded sidecar"
    assert decoded.suffix == ".yuv"
    assert decoded.exists()
    assert captured, "ffmpeg runner was never invoked"
    cmd = captured[0]
    assert "-i" in cmd and str(src) in cmd
    assert "-f" in cmd and "rawvideo" in cmd
    assert "-t" in cmd, "duration_s clamp must be threaded through"
    assert float(cmd[cmd.index("-t") + 1]) == 4.0


def test_maybe_decode_reference_passthrough_for_raw(tmp_path: Path) -> None:
    """Raw ``.yuv`` references are a no-op (returncode 0, path unchanged)."""
    captured: list[list[str]] = []

    def _runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        captured.append(list(argv))
        return _FakeCompleted(returncode=0)

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 1024)

    decoded, rc = _maybe_decode_reference(
        src,
        encode_dir=tmp_path / "encodes",
        pix_fmt="yuv420p",
        duration_s=0.0,
        ffmpeg_bin="ffmpeg",
        runner=_runner,
    )
    assert rc == 0
    assert decoded == src
    assert not captured, "raw YUV must not invoke ffmpeg"


def test_maybe_decode_reference_caches_across_calls(tmp_path: Path) -> None:
    """Repeat calls with an already-present sidecar must not re-run ffmpeg.

    The corpus pipeline decodes once per ``iter_rows`` call and reuses
    the decoded sidecar across every (preset, crf) cell. Pinning this
    avoids regressing into per-cell decodes — that was the cost
    rationale in ADR-0499 over the naive "decode in every score call"
    placement.
    """
    invocations: list[list[str]] = []

    def _runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        invocations.append(list(argv))
        out_path = Path(argv[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0)

    src = tmp_path / "ref.mp4"
    src.write_bytes(b"\x00")
    encode_dir = tmp_path / "encodes"

    _decoded1, rc1 = _maybe_decode_reference(
        src,
        encode_dir=encode_dir,
        pix_fmt="yuv420p",
        duration_s=2.0,
        ffmpeg_bin="ffmpeg",
        runner=_runner,
    )
    assert rc1 == 0
    assert len(invocations) == 1

    _decoded2, rc2 = _maybe_decode_reference(
        src,
        encode_dir=encode_dir,
        pix_fmt="yuv420p",
        duration_s=2.0,
        ffmpeg_bin="ffmpeg",
        runner=_runner,
    )
    assert rc2 == 0
    assert len(invocations) == 1, "second call must hit the on-disk cache"


def test_ladder_decodes_reference_for_container_source(tmp_path: Path) -> None:
    """End-to-end pin: an .mp4 reference triggers a reference decode.

    Drives :func:`vmaftune.corpus.iter_rows` with a one-cell job whose
    source is an ``.mp4``. Mocks the encode runner to materialise the
    distorted output and asserts the score runner sees a ``.yuv``
    path for both ``--reference`` and ``--distorted``. Pre-ADR-0499
    the reference argv slot still held the ``.mp4`` path and the vmaf
    binary aborted with a file-size mismatch.
    """
    src = tmp_path / "ref.mp4"
    src.write_bytes(b"\x00")
    encode_dir = tmp_path / "encodes"

    score_captures: list[list[str]] = []

    def _encode_runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        # Synthesize the distorted output so the corpus pipeline thinks
        # the encode succeeded.
        # ffmpeg argv ends with the output path.
        out_path = Path(argv[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 4096)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.0\n")

    def _score_runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        score_captures.append(list(argv))
        # Write a minimal JSON the vmaf parser will accept.
        if "--output" in argv:
            out_idx = argv.index("--output") + 1
            json_path = Path(argv[out_idx])
            json_path.parent.mkdir(parents=True, exist_ok=True)
            json_path.write_text(
                '{"pooled_metrics": {"vmaf": {"mean": 95.5}}}',
                encoding="utf-8",
            )
        return _FakeCompleted(returncode=0, stderr="VMAF version 3.0.0\n")

    # The reference decode + distorted decode steps both shell out via
    # ``subprocess.run`` directly (the score/encode runner injection
    # only covers the vmaf and ffmpeg-encode CLIs respectively). Patch
    # ``subprocess.run`` to materialise the requested raw-YUV sidecars
    # so the corpus pipeline can proceed.
    import subprocess as _sp

    def _decode_runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        out_path = Path(argv[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 4096)
        return _FakeCompleted(returncode=0)

    original_run = _sp.run
    _sp.run = _decode_runner

    job = CorpusJob(
        source=src,
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=5.0,
        cells=(("medium", 28),),
    )
    opts = CorpusOptions(
        encoder="libx264",
        output=tmp_path / "corpus.jsonl",
        encode_dir=encode_dir,
        src_sha256=False,
    )

    try:
        rows = list(
            iter_rows(
                job,
                opts,
                encode_runner=_encode_runner,
                score_runner=_score_runner,
            )
        )
    finally:
        _sp.run = original_run

    assert len(rows) == 1
    row = rows[0]
    assert row["exit_status"] == 0, f"row failed: {row.get('vmaf_score')!r}"
    assert score_captures, "vmaf CLI was never invoked — sampler short-circuited"

    # Both --reference and --distorted paths handed to the vmaf binary
    # must be raw YUV (suffix-checked, not extension-stripped).
    cmd = score_captures[0]
    ref_idx = cmd.index("--reference") + 1
    dist_idx = cmd.index("--distorted") + 1
    ref_path = Path(cmd[ref_idx])
    dist_path = Path(cmd[dist_idx])
    assert (
        ref_path.suffix.lower() in _VMAF_RAW_SUFFIXES
    ), f"reference handed to vmaf must be raw YUV; got {ref_path}"
    assert (
        dist_path.suffix.lower() in _VMAF_RAW_SUFFIXES
    ), f"distorted handed to vmaf must be raw YUV; got {dist_path}"
    # The pre-ADR-0499 bug specifically left the source .mp4 in the
    # --reference slot. Pin against that exact shape.
    assert ref_path != src, "container reference must be decoded before being handed to vmaf"


def test_reference_decode_failure_fails_every_cell_cleanly(tmp_path: Path) -> None:
    """When the reference decode fails, every cell records exit_status != 0.

    Pins the short-circuit path: the encode + vmaf invocations are
    skipped (avoids re-running ffmpeg N times on output we cannot
    score), the row carries the decode failure context, and the
    sampler sees a structured failure rather than a crash.
    """
    src = tmp_path / "broken.mp4"
    src.write_bytes(b"\x00")

    encode_calls: list[list[str]] = []
    score_calls: list[list[str]] = []

    def _decode_failing_runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        # Reference decode fails; never write the output file.
        return _FakeCompleted(returncode=1, stderr="ffmpeg: invalid data\n")

    def _encode_runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        encode_calls.append(list(argv))
        return _FakeCompleted(returncode=0)

    def _score_runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        score_calls.append(list(argv))
        return _FakeCompleted(returncode=0)

    # Patch subprocess.run inside corpus.py so the reference-decode
    # ffmpeg call (which uses subprocess.run directly) fails. The
    # encode_runner / score_runner injection covers the test-friendly
    # paths; the reference decode runs through subprocess.run because
    # score stubs only mock vmaf calls.
    import subprocess as _sp

    original_run = _sp.run
    _sp.run = _decode_failing_runner
    try:
        job = CorpusJob(
            source=src,
            width=1920,
            height=1080,
            pix_fmt="yuv420p",
            framerate=30.0,
            duration_s=5.0,
            cells=(("medium", 23), ("medium", 28)),
        )
        opts = CorpusOptions(
            encoder="libx264",
            output=tmp_path / "corpus.jsonl",
            encode_dir=tmp_path / "encodes",
            src_sha256=False,
        )
        rows = list(
            iter_rows(
                job,
                opts,
                encode_runner=_encode_runner,
                score_runner=_score_runner,
            )
        )
    finally:
        _sp.run = original_run

    assert len(rows) == 2
    for row in rows:
        assert row["exit_status"] != 0
    assert not encode_calls, "encode must be skipped when reference decode fails"
    assert not score_calls, "vmaf must be skipped when reference decode fails"


# ---------------------------------------------------------------------------
# Bug #V3-B (suffix table) — ``.y4m`` must NOT be treated as raw YUV
# ---------------------------------------------------------------------------


def test_vmaf_raw_suffixes_matches_cli_acceptance() -> None:
    """Pin the suffix tables against what the libvmaf CLI actually accepts.

    vmaf-tune always passes ``--width`` / ``--height`` /
    ``--pixel_format`` / ``--bitdepth``; the CLI's ``cli_parse.c``
    flips ``settings->use_yuv = true`` on each of those flags
    (core/tools/cli_parse.c lines 637-651 as of 2026-05-18) which
    routes both inputs through ``raw_input_open`` — never through
    Y4M auto-detection. ``.y4m`` files therefore fail with a file-
    size mismatch and must NOT be listed as "no decode needed".
    """
    expected = frozenset({".yuv", ""})
    assert (
        _VMAF_RAW_SUFFIXES == expected
    ), f"corpus._VMAF_RAW_SUFFIXES drifted: {_VMAF_RAW_SUFFIXES}"
    assert VMAF_RAW_SUFFIXES == expected, f"score.VMAF_RAW_SUFFIXES drifted: {VMAF_RAW_SUFFIXES}"
    assert ".y4m" not in _VMAF_RAW_SUFFIXES, ".y4m must trigger a decode — see ADR-0499"
    assert ".y4m" not in VMAF_RAW_SUFFIXES, ".y4m must trigger a decode — see ADR-0499"


def test_vmaf_raw_suffixes_matches_libvmaf_cli_source() -> None:
    """Cross-check the suffix table against the libvmaf CLI source.

    Walks ``core/tools/cli_parse.c`` and asserts that the flags
    vmaf-tune emits unconditionally (``-w`` / ``-h`` / ``-p`` / ``-b``)
    still set ``use_yuv = true``. If upstream ever changes the
    surface to accept Y4M via extension auto-detect even with explicit
    width/height/pixfmt, this test fails loudly so the suffix table
    can be revised in lockstep.
    """
    repo_root = Path(__file__).resolve().parents[3]
    cli_src = repo_root / "libvmaf" / "tools" / "cli_parse.c"
    if not cli_src.exists():
        # Tests live inside vmaf-tune subtree; allow soft-skip when
        # extracted into a standalone wheel test run.
        import pytest

        pytest.skip(f"libvmaf source not available at {cli_src}")

    text = cli_src.read_text(encoding="utf-8", errors="replace")
    # Every one of -w/-h/-p/-b must set use_yuv true. Use a permissive
    # pattern so re-formatting (clang-format pass) doesn't false-fail.
    for short_opt in ("'w'", "'h'", "'p'", "'b'"):
        # Find the case arm; require ``use_yuv = true`` inside it.
        m = re.search(
            rf"case\s+{re.escape(short_opt)}\s*:(.*?)break;",
            text,
            re.DOTALL,
        )
        assert m is not None, f"case {short_opt} not found in cli_parse.c"
        body = m.group(1)
        assert (
            "use_yuv" in body and "true" in body
        ), f"case {short_opt} no longer sets use_yuv=true; suffix table needs revisit"


# ---------------------------------------------------------------------------
# Bug #V3-B (bisect parity) — bisect path also decodes the reference
# ---------------------------------------------------------------------------


def test_bisect_decodes_reference_too() -> None:
    """The bisect path already decodes the reference (ADR-0498 v2).

    V3 verified the bisect handles container references correctly via
    its own ``_decode_to_raw_yuv`` call before scoring. Pin that the
    code path still exists so an over-zealous "delete dead code"
    refactor doesn't regress us into the corpus-only regime.
    """
    bisect_src = Path(__file__).resolve().parents[1] / "src" / "vmaftune" / "bisect.py"
    text = bisect_src.read_text(encoding="utf-8")
    assert (
        "src_is_container" in text
    ), "bisect.py no longer detects container references — ADR-0499 invariant"
    assert (
        "_decode_to_raw_yuv" in text
    ), "bisect.py no longer decodes the reference — ADR-0499 invariant"
    assert (
        "decoded_ref" in text
    ), "bisect.py no longer carries the decoded-ref sidecar — ADR-0499 invariant"


# ---------------------------------------------------------------------------
# Bug #V3-C — encoder unavailable in container build is flagged cleanly
# ---------------------------------------------------------------------------


def test_compare_marks_encoder_unavailable_distinctly() -> None:
    """The bisect predicate distinguishes "missing encoder" from "encode failed".

    Pinned regression for the V3-C informational finding: the
    container's ffmpeg ships without libsvtav1 (ADR-0496). The error
    message must say "encoder unavailable", not the cryptic
    "encode failed: Encoder not found". The discrimination logic
    lives in :func:`vmaftune.bisect._predicate_for_codec` (line 456-461
    as of ADR-0498).
    """
    bisect_src = Path(__file__).resolve().parents[1] / "src" / "vmaftune" / "bisect.py"
    text = bisect_src.read_text(encoding="utf-8")
    assert (
        "encoder unavailable" in text
    ), "ADR-0499 invariant: bisect must classify missing encoders explicitly"
    assert (
        "encoder not found" in text.lower()
    ), "ADR-0499 invariant: bisect must scan stderr for 'Encoder not found'"
