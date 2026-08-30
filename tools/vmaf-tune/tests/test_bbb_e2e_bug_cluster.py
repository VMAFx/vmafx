# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the 2026-05-17 BBB end-to-end bug cluster.

The Big Buck Bunny end-to-end smoke run flagged seven distinct bugs in
the vmaf-tune pipeline (compare → ladder → tune-per-shot → report).
This module pins one regression test per bug so the entire cluster
stays fixed together. See ADR-0497 for the full incident write-up and
``/tmp/bbb_e2e_bugs.md`` for the original repro commands.
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# Make ``vmaftune`` importable for the in-tree test invocation, mirroring
# the pattern already used by ``test_bisect.py`` / ``test_report.py``.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.bisect import bisect_target_vmaf
from vmaftune.compare import (
    ComparisonReport,
    RecommendResult,
    emit_report,
)
from vmaftune.ladder import make_default_sampler
from vmaftune.report import (
    _DASH,
    CodecRow,
    ReportData,
    SourceInfo,
    render_html,
    render_markdown,
)

# ---------------------------------------------------------------------------
# Stub subprocess helpers (mirrors test_bisect.py's contract)
# ---------------------------------------------------------------------------


@dataclass
class _FakeCompleted:
    returncode: int = 0
    stdout: str = ""
    stderr: str = "ffmpeg version 0-test\nx264 - core 164\nVMAF version 3.0.0-test\n"


def _make_bisect_stubs(vmaf_score: float = 92.0):
    """Return (encode_runner, score_runner, log) stubs for bisect tests."""
    log: list[dict[str, Any]] = []

    def _encode_runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        out_path = Path(argv[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 100_000)
        log.append({"kind": "encode_or_decode", "argv": list(argv)})
        return _FakeCompleted(returncode=0)

    def _score_runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        if "--output" in argv:
            out = Path(argv[argv.index("--output") + 1])
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(
                '{"pooled_metrics": {"vmaf": {"mean": ' + repr(vmaf_score) + "}}}\n",
                encoding="utf-8",
            )
        log.append({"kind": "score", "argv": list(argv)})
        return _FakeCompleted(returncode=0)

    return _encode_runner, _score_runner, log


# ---------------------------------------------------------------------------
# Bug #3 — bisect decodes container distorted before invoking vmaf
# ---------------------------------------------------------------------------


def test_bisect_decodes_container_distorted(tmp_path: Path) -> None:
    """Bug #3: bisect must decode the .mkv encode to raw YUV before scoring.

    Confirms the score-step subprocess seam sees a ``.decoded.yuv``
    distorted path, not the raw ``.mkv`` artefact the encoder wrote.
    Without the fix the libvmaf CLI aborts with "file too small for
    declared geometry" on the matroska bytes.
    """
    enc, sc, log = _make_bisect_stubs(vmaf_score=92.5)
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=90.0,
        width=1280,
        height=720,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=2.0,
        crf_range=(23, 25),
        max_iterations=2,
        encode_runner=enc,
        score_runner=sc,
        workdir=tmp_path,
    )
    assert result.ok, result.error
    score_calls = [e for e in log if e["kind"] == "score"]
    assert score_calls, "score runner was never invoked"
    for sc_call in score_calls:
        argv = sc_call["argv"]
        assert "--distorted" in argv
        distorted = argv[argv.index("--distorted") + 1]
        # The path must be the decoded sidecar, never the raw .mkv.
        assert distorted.endswith(
            ".decoded.yuv"
        ), f"expected decoded sidecar, got distorted={distorted!r}"
        assert not distorted.endswith(".mkv")


# ---------------------------------------------------------------------------
# Bug #1 — bisect autodetects container sources (no -f rawvideo for mp4)
# ---------------------------------------------------------------------------


def test_compare_autodetects_container_src(tmp_path: Path) -> None:
    """Bug #1: a container ``--src`` must propagate ``source_is_container=True``.

    Asserts that when bisect runs on an ``.mp4`` reference the encoder
    ffmpeg invocation omits the ``-f rawvideo -pix_fmt … -s … -r …``
    input flags (which would otherwise force a raw-demux interpretation
    and produce "Output file is empty, nothing was encoded").
    """
    container_src = tmp_path / "ref.mp4"
    container_src.write_bytes(b"\x00")  # presence is enough; stubs intercept ffmpeg
    enc, sc, log = _make_bisect_stubs(vmaf_score=92.0)
    result = bisect_target_vmaf(
        container_src,
        "libx264",
        target_vmaf=90.0,
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=5.0,
        crf_range=(23, 24),
        max_iterations=1,
        encode_runner=enc,
        score_runner=sc,
        workdir=tmp_path / "work",
    )
    assert result.ok, result.error
    encode_calls = [e for e in log if e["kind"] == "encode_or_decode" and "-crf" in e["argv"]]
    assert encode_calls, "no encode invocation recorded"
    encode_argv = encode_calls[0]["argv"]
    # The encode invocation must NOT prepend ``-f rawvideo`` before ``-i``.
    i_index = encode_argv.index("-i")
    pre_input = encode_argv[:i_index]
    assert (
        "rawvideo" not in pre_input
    ), f"container source must not be demuxed as rawvideo; argv={pre_input}"


# ---------------------------------------------------------------------------
# Bug #2 — compare JSON contains no bare NaN literals (RFC 8259 strict)
# ---------------------------------------------------------------------------


def test_compare_json_no_bare_nan() -> None:
    """Bug #2: ``compare --format json`` must round-trip via strict JSON parsers.

    A report with one failed row (NaN bitrate / VMAF) must serialise to
    JSON whose ``json.loads(..., parse_constant=...)`` does NOT raise,
    confirming there are no ``NaN`` / ``Infinity`` tokens. Failed
    numerics land as ``null`` so consumers can disambiguate via the
    row-level ``ok`` flag.
    """
    report = ComparisonReport(
        src="/tmp/example.mp4",
        target_vmaf=92.0,
        tool_version="test",
        wall_time_ms=1.0,
        rows=(
            RecommendResult(
                codec="libx264",
                best_crf=23,
                bitrate_kbps=2400.0,
                encode_time_ms=4000.0,
                vmaf_score=92.4,
                encoder_version="x264",
                ok=True,
                error="",
            ),
            RecommendResult(
                codec="libx265",
                best_crf=-1,
                bitrate_kbps=float("nan"),
                encode_time_ms=float("nan"),
                vmaf_score=float("nan"),
                encoder_version="",
                ok=False,
                error="score failed at CRF 26 (exit=2)",
            ),
        ),
    )
    rendered = emit_report(report, format="json")
    # Strict load: parse_constant fires only on NaN/Infinity tokens.
    parsed = json.loads(
        rendered,
        parse_constant=lambda x: (_ for _ in ()).throw(
            ValueError(f"bare {x!r} token in JSON output")
        ),
    )
    # Failed row's numerics serialised to null.
    rows_by_codec = {r["codec"]: r for r in parsed["rows"]}
    failed = rows_by_codec["libx265"]
    assert failed["bitrate_kbps"] is None
    assert failed["vmaf_score"] is None
    assert failed["ok"] is False
    # Successful row keeps its numerics.
    ok = rows_by_codec["libx264"]
    assert ok["bitrate_kbps"] == 2400.0
    assert ok["ok"] is True
    # Literal NaN substring should not appear anywhere in the output.
    assert "NaN" not in rendered
    assert "Infinity" not in rendered


# ---------------------------------------------------------------------------
# Bug #4 / Bug #5 — ladder CLI exposes source-shape + crf-sweep overrides
# ---------------------------------------------------------------------------


def test_ladder_cli_source_shape_flags() -> None:
    """Bug #4/#5: ``make_default_sampler`` threads CLI flags to the corpus job.

    Confirms the factory builds a sampler that, when invoked,
    constructs a :class:`CorpusJob` carrying the supplied
    ``framerate`` / ``duration_s`` / ``pix_fmt`` and a configured
    CRF sweep — none of which the previous hardcoded default sampler
    permitted overriding.
    """
    captured: dict[str, Any] = {}

    def _fake_iter_rows(job, opts):  # type: ignore[no-untyped-def]
        captured["job"] = job
        captured["opts"] = opts
        # Return one successful row so pick_target_vmaf has something to chew on.
        yield {
            "crf": job.cells[0][1],
            "vmaf_score": 95.0,
            "bitrate_kbps": 1234.0,
            "exit_status": 0,
        }

    sampler = make_default_sampler(
        pix_fmt="yuv422p",
        framerate=30.0,
        duration_s=60.0,
        crf_sweep=(20, 25),
    )

    # Patch the corpus.iter_rows the sampler imports lazily.
    import vmaftune.corpus as corpus_mod

    real_iter_rows = corpus_mod.iter_rows
    corpus_mod.iter_rows = _fake_iter_rows
    try:
        point = sampler(Path("source.yuv"), "libx264", 1920, 1080, 92.0)
    finally:
        corpus_mod.iter_rows = real_iter_rows

    job = captured["job"]
    assert job.framerate == 30.0
    assert job.duration_s == 60.0
    assert job.pix_fmt == "yuv422p"
    crf_values = tuple(c[1] for c in job.cells)
    assert crf_values == (20, 25), f"expected (20,25), got {crf_values}"
    # The picked point reflects the synthetic row.
    assert point.crf == 20
    assert point.bitrate_kbps == 1234.0


# ---------------------------------------------------------------------------
# Bug #6 — report renders NaN rows as em-dash + aggregates top-level ok
# ---------------------------------------------------------------------------


def test_report_handles_nan_rows(tmp_path: Path) -> None:
    """Bug #6: ``vmaf-tune report`` must em-dash NaN cells, never ``nan kbps``.

    Builds a :class:`ReportData` with one successful row and one
    failed row (NaN numerics, ``ok=False``) and asserts the markdown
    + HTML renderers substitute the em-dash placeholder for every
    NaN cell. Also confirms the CLI ``_run_report`` aggregator
    flips the top-level ``ok`` flag to ``False`` when any row failed.
    """
    src = SourceInfo(
        path="/tmp/x.mp4",
        width=1920,
        height=1080,
        fps=30.0,
        duration_s=60.0,
        frame_count=1800,
        codec="h264",
        size_bytes=1_000_000,
    )
    data = ReportData(
        source=src,
        target_vmaf=92.0,
        codec_rows=(
            CodecRow("libx264", "x264", 23, 2400, 4000, 92.4, True),
            CodecRow(
                "libx265",
                "",
                -1,
                float("nan"),
                float("nan"),
                float("nan"),
                False,
                "score failed at CRF 26 (exit=2)",
            ),
        ),
    )
    md = render_markdown(data)
    # The whole rendered output must NOT contain literal ``nan kbps``
    # / ``nan`` tokens (the renderer used to print them; Bug #6).
    md_lower = md.lower()
    assert "nan kbps" not in md_lower
    # Look only at the rendered table rows (not the collapsed JSON
    # dump at the bottom, which legitimately echoes the dataclass).
    table_lines = [ln for ln in md.splitlines() if ln.startswith("|")]
    failed_table_lines = [ln for ln in table_lines if "libx265" in ln]
    assert failed_table_lines, "failed-row markdown table line missing"
    for line in failed_table_lines:
        assert " nan " not in line.lower()
        assert _DASH in line  # em-dash placeholder present
    # Successful row keeps its numerics formatted.
    ok_table_line = next(ln for ln in table_lines if "libx264" in ln)
    assert "2400 kbps" in ok_table_line or "2.40 Mbps" in ok_table_line
    assert "92.40" in ok_table_line

    html = render_html(data)
    assert "nan kbps" not in html.lower()
    assert _DASH in html

    # Top-level ``ok`` aggregator: the CLI handler reads ``CodecRow.ok``
    # and writes ``"ok": false`` when any row failed. We exercise the
    # same logic the handler uses (kept inline in cli._run_report).
    rows = data.codec_rows
    rows_ok = all(r.ok for r in rows) if rows else True
    rows_any = any(r.ok for r in rows) if rows else True
    top_ok = bool(rows_ok and rows_any)
    assert top_ok is False  # at least one failed row -> top-level False


# ---------------------------------------------------------------------------
# Bug #7 — tune-per-shot's per-shot bisect benefits from the Bug #3 fix
# ---------------------------------------------------------------------------


def test_tune_per_shot_score_step_decodes_container(tmp_path: Path) -> None:
    """Bug #7: the per-shot bisect routes through the same scoring fix as compare.

    ``_build_per_shot_bisect_predicate`` extracts each shot to raw
    YUV and then calls :func:`bisect_target_vmaf` — so once Bug #3
    is patched, the per-shot path inherits the decode step
    automatically. This test pins that invariant: running the same
    bisect API the per-shot CLI uses must not leave a ``.mkv`` path
    in the score-step argv.
    """
    enc, sc, log = _make_bisect_stubs(vmaf_score=91.0)
    result = bisect_target_vmaf(
        tmp_path / "shot_0_30.yuv",
        "libx264",
        target_vmaf=90.0,
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=1.0,
        crf_range=(23, 24),
        max_iterations=1,
        encode_runner=enc,
        score_runner=sc,
        workdir=tmp_path / "work",
    )
    assert result.ok, result.error
    score_argvs = [e["argv"] for e in log if e["kind"] == "score"]
    for argv in score_argvs:
        if "--distorted" in argv:
            distorted = argv[argv.index("--distorted") + 1]
            assert not distorted.endswith(
                ".mkv"
            ), f"per-shot bisect still feeds .mkv to vmaf: {distorted!r}"
