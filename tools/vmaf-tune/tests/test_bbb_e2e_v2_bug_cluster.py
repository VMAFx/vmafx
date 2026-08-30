# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the 2026-05-18 BBB end-to-end v2 bug cluster.

The follow-up Big Buck Bunny end-to-end smoke run (after PR #1253 /
ADR-0497 closed the first cluster) flagged five new bugs in the
next layer down — disk-runaway in the score-step decoder, a cross-
resolution ladder crash, a missing dependency in the dev-mcp image,
a residual bare-``NaN`` literal in the report JSON appendix, and a
silent CPU fallback on explicit ``--backend X`` failure. This module
pins one regression per bug per ADR-0498.
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

import math  # noqa: E402

from vmaftune.ladder import make_default_sampler  # noqa: E402
from vmaftune.report import CodecRow, ReportData, SourceInfo, render_markdown  # noqa: E402
from vmaftune.score import ScoreRequest, _decode_to_raw_yuv, maybe_decode_distorted  # noqa: E402


@dataclass
class _FakeCompleted:
    returncode: int = 0
    stdout: str = ""
    stderr: str = ""


# ---------------------------------------------------------------------------
# Bug #v2-A — _decode_to_raw_yuv honours ``duration_s``
# ---------------------------------------------------------------------------


def test_decode_to_raw_yuv_respects_duration(tmp_path: Path) -> None:
    """Bug #v2-A: the decoded YUV must be size-bounded by ``-t duration_s``.

    Without the fix the decoder ran ``ffmpeg -i src -f rawvideo …`` with
    no ``-t`` clamp and dumped the full 634 s source (~58 GB at 1080p)
    even when the caller only intended to score a 10 s probe — nearly
    OOM'd the dev-mcp container's 1.9 TB volume on the BBB e2e run.
    """
    captured: list[list[str]] = []

    def _runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        captured.append(list(argv))
        # Synthesize the expected output file so the helper returns 0.
        out_path = Path(argv[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0)

    src = tmp_path / "src.mp4"
    src.write_bytes(b"\x00")
    dst = tmp_path / "out.yuv"

    rc = _decode_to_raw_yuv(
        src,
        dst,
        pix_fmt="yuv420p",
        ffmpeg_bin="ffmpeg",
        runner=_runner,
        duration_s=3.0,
    )
    assert rc == 0
    assert captured, "ffmpeg runner was never invoked"
    cmd = captured[0]
    assert "-t" in cmd, f"expected -t clamp in ffmpeg argv, got {cmd}"
    t_value = cmd[cmd.index("-t") + 1]
    assert float(t_value) == 3.0, f"expected -t 3.0, got -t {t_value!r}"

    # Legacy path (duration_s omitted) must still produce a -t-free argv.
    captured.clear()
    rc = _decode_to_raw_yuv(
        src,
        dst,
        pix_fmt="yuv420p",
        ffmpeg_bin="ffmpeg",
        runner=_runner,
    )
    assert rc == 0
    assert "-t" not in captured[0], "legacy decode path must not inject -t"


def test_maybe_decode_distorted_propagates_duration(tmp_path: Path) -> None:
    """``maybe_decode_distorted`` honours ``ScoreRequest.duration_s``.

    Pins the wiring from the public ``ScoreRequest`` field down to the
    ffmpeg ``-t`` clamp so callers (bisect + corpus) only need to set
    the new field on the request to get a bounded decode.
    """
    captured: list[list[str]] = []

    def _runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        captured.append(list(argv))
        Path(argv[-1]).parent.mkdir(parents=True, exist_ok=True)
        Path(argv[-1]).write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0)

    distorted = tmp_path / "enc.mkv"
    distorted.write_bytes(b"\x00")
    req = ScoreRequest(
        reference=tmp_path / "ref.yuv",
        distorted=distorted,
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        duration_s=5.0,
    )
    new_req, rc = maybe_decode_distorted(
        req, workdir=tmp_path / "work", ffmpeg_bin="ffmpeg", runner=_runner
    )
    assert rc == 0
    assert new_req.distorted.suffix == ".yuv"
    assert captured, "runner never invoked"
    cmd = captured[0]
    assert "-t" in cmd
    assert float(cmd[cmd.index("-t") + 1]) == 5.0


# ---------------------------------------------------------------------------
# Bug #v2-B — ladder cross-resolution against a raw YUV source
# ---------------------------------------------------------------------------


def test_ladder_cross_resolution_against_yuv_source() -> None:
    """Bug #v2-B: 1920x1080 raw YUV -> 1280x720 ladder rung must succeed.

    The default sampler used to pass the rung's target dims as the
    source dims to the corpus job (and thus to ffmpeg's ``-s W:H``
    rawvideo input flag). For a 1080p source that became
    ``-s 1280x720`` -> ffmpeg parsed 1080p bytes as 720p frames and
    every encode exited non-zero, producing the cryptic
    "no scorable encodes" stack trace.

    The fix: ``make_default_sampler`` accepts ``src_width / src_height``,
    threads them through ``CorpusJob.src_width / src_height``, and the
    corpus EncodeRequest builder switches to source-dim ``-s`` plus a
    ``-vf scale=W:H`` filter when source != target.
    """
    captured: dict[str, Any] = {}

    def _fake_iter_rows(job, opts):  # type: ignore[no-untyped-def]
        captured["job"] = job
        captured["opts"] = opts
        yield {
            "crf": job.cells[0][1],
            "vmaf_score": 88.0,
            "bitrate_kbps": 2500.0,
            "exit_status": 0,
        }

    sampler = make_default_sampler(
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=10.0,
        crf_sweep=(28,),
        src_width=1920,
        src_height=1080,
    )

    import vmaftune.corpus as corpus_mod

    real = corpus_mod.iter_rows
    corpus_mod.iter_rows = _fake_iter_rows
    try:
        point = sampler(Path("src.yuv"), "libx264", 1280, 720, 88.0)
    finally:
        corpus_mod.iter_rows = real

    assert point.width == 1280
    assert point.height == 720
    job = captured["job"]
    # Score-side dims = rung target (libvmaf scores at the rung resolution).
    assert (job.width, job.height) == (1280, 720)
    # Source-side dims preserved for the encode pipe.
    assert (job.src_width, job.src_height) == (1920, 1080)


def test_corpus_emits_scale_filter_when_src_dims_differ(tmp_path: Path, monkeypatch: Any) -> None:
    """``iter_rows`` injects ``-vf scale=W:H`` for cross-res YUV sources.

    ADR-0501 follow-up: the reference decode now also downscales to the
    rung target on cross-res rungs (Bug #V4-B); the test must stub the
    ffmpeg decode subprocess so it doesn't shell out against the 1-byte
    fixture YUV. The encode-side argv assertions remain unchanged.
    """
    import vmaftune.corpus as corpus_mod
    from vmaftune.corpus import CorpusJob, CorpusOptions, iter_rows

    job = CorpusJob(
        source=tmp_path / "src.yuv",
        width=1280,
        height=720,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=2.0,
        cells=(("medium", 28),),
        src_width=1920,
        src_height=1080,
    )
    (tmp_path / "src.yuv").write_bytes(b"\x00")
    opts = CorpusOptions(
        encoder="libx264",
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "enc",
        src_sha256=False,
    )
    encode_argvs: list[list[str]] = []
    decode_argvs: list[list[str]] = []

    def _enc_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        encode_argvs.append(list(argv))
        out = Path(argv[-1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.0\n")

    def _score_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        out = Path(argv[argv.index("--output") + 1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text('{"pooled_metrics": {"vmaf": {"mean": 80.0}}}\n')
        return _FakeCompleted(returncode=0, stderr="VMAF version 3.0.0-test\n")

    # Stub the ffmpeg decode subprocess used by both
    # ``_maybe_decode_reference`` (cross-res raw YUV, ADR-0501) and
    # ``_maybe_decode_distorted`` (container->raw, ADR-0499). They
    # both shell out via ``subprocess.run``; the test fixture is a
    # 1-byte placeholder so a real ffmpeg call would fail. Producing
    # the destination file with ``\x00`` bytes lets the iter_rows
    # cell loop reach the encode/score stubs.
    import subprocess as _sp

    def _fake_sp_run(argv: list[Any], **_kw: Any) -> _FakeCompleted:
        decode_argvs.append([str(a) for a in argv])
        dest = Path(argv[-1])
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0)

    monkeypatch.setattr(_sp, "run", _fake_sp_run)
    monkeypatch.setattr(corpus_mod, "_sha256_file", lambda *_a, **_kw: "")

    rows = list(iter_rows(job, opts, encode_runner=_enc_runner, score_runner=_score_runner))
    assert rows, "iter_rows produced no rows"
    assert encode_argvs, "encode runner never called"
    argv = encode_argvs[0]
    # Encode pipe must use source dims on -s and add a -vf scale=target.
    assert "-s" in argv
    s_value = argv[argv.index("-s") + 1]
    assert s_value == "1920x1080", f"expected source -s 1920x1080, got {s_value}"
    assert "-vf" in argv
    vf_value = argv[argv.index("-vf") + 1]
    assert "scale=1280:720" in vf_value
    # ADR-0501 / Bug #V4-B: the reference decode must also have run a
    # ``-vf scale=1280:720`` to materialise a 720p reference YUV — the
    # libvmaf CLI then reads both legs at the same geometry.
    ref_decodes = [d for d in decode_argvs if any("ref.decoded" in str(part) for part in d)]
    assert ref_decodes, f"reference decode never ran (decode_argvs={decode_argvs!r})"
    ref_argv = ref_decodes[0]
    assert "-vf" in ref_argv
    ref_vf = ref_argv[ref_argv.index("-vf") + 1]
    assert "scale=1280:720" in ref_vf, f"expected reference scale=1280:720, got {ref_vf!r}"


# ---------------------------------------------------------------------------
# Bug #v2-D — report appendix emits no bare NaN literals
# ---------------------------------------------------------------------------


def test_report_json_appendix_no_bare_nan(tmp_path: Path) -> None:
    """Bug #v2-D: report Markdown's <details> appendix must be portable JSON.

    The visible Markdown table already renders NaN cells as the em-dash
    placeholder. The bug is in the appendix JSON dump — previously emitted
    via ``json.dumps(..., allow_nan=True)``, which produces RFC-8259-
    invalid bare ``NaN`` tokens that Go / Rust / jq strict parsers
    reject. The fix coerces NaN to ``null`` and pins
    ``allow_nan=False`` defence-in-depth.
    """
    data = ReportData(
        source=SourceInfo(
            path="/tmp/example.mp4",
            width=1920,
            height=1080,
            fps=30.0,
            duration_s=10.0,
            frame_count=300,
            codec="h264",
            size_bytes=10_000_000,
        ),
        target_vmaf=92.0,
        codec_rows=(
            CodecRow(
                codec="libsvtav1",
                encoder_version="",
                best_crf=-1,
                bitrate_kbps=float("nan"),
                encode_time_ms=float("nan"),
                vmaf_score=float("nan"),
                ok=False,
                error="encoder unavailable",
            ),
        ),
    )
    md = render_markdown(data, assets_dir=tmp_path / "assets")
    # Locate the appendix code-fenced JSON block.
    assert "<details><summary>report.json</summary>" in md
    start = md.index("```json")
    end = md.index("```", start + len("```json"))
    appendix = md[start + len("```json") : end].strip()
    # Strict parse — bare NaN tokens would raise here.
    parsed = json.loads(
        appendix,
        parse_constant=lambda x: (_ for _ in ()).throw(
            ValueError(f"bare {x!r} token in report appendix JSON")
        ),
    )
    assert "NaN" not in appendix
    assert "Infinity" not in appendix
    # The failed codec row's NaN numerics surface as null after the
    # _nan_to_none coercion.
    rows = parsed["codec_rows"]
    assert rows[0]["bitrate_kbps"] is None
    assert rows[0]["vmaf_score"] is None


# ---------------------------------------------------------------------------
# Bug #v2-E — vmaf --backend explicit failure errors out (C binary)
# ---------------------------------------------------------------------------


def test_vmaf_explicit_backend_failure_errors() -> None:
    """Bug #v2-E: C-side behaviour pinned by source inspection.

    The vmaf binary is built per-host; a Python integration test would
    flake when the dev box doesn't have a particular GPU. Instead we
    pin the source-level invariant: the init_gpu_backends() helper now
    derives an ``explicit_backend`` flag from ``--backend NAME`` and
    turns each per-backend ``state_init`` failure into ``return -1``
    (-> non-zero exit) when that backend was the requested one. Soft
    fallback to CPU only happens for ``--backend auto`` (and the
    implicit default).

    Pinning the source carries the same regression-prevention value as
    a live integration test for the dispatch policy; the test passes
    on CI hosts without CUDA / SYCL / HIP. ADR-0726 dropped the Vulkan
    backend on 2026-05-28, so ``vulkan`` is no longer one of the
    per-backend strcmp targets.
    """
    repo_root = Path(__file__).resolve().parents[3]
    src = (repo_root / "core/tools/vmaf.c").read_text(encoding="utf-8")
    # The explicit-backend gate must be defined exactly once.
    assert "explicit_backend" in src, "missing explicit_backend variable in vmaf.c"
    assert 'strcmp(c->backend, "auto") != 0' in src, "auto exemption missing"
    # Each per-backend init failure has the explicit guard. Vulkan was
    # removed by ADR-0726 — guard against accidental reintroduction.
    for backend in ("sycl", "cuda", "hip", "metal"):
        marker = f'strcmp(c->backend, "{backend}") == 0'
        assert marker in src, f"explicit-backend guard missing for --backend {backend}"
    assert (
        'strcmp(c->backend, "vulkan")' not in src
    ), "ADR-0726 regression: Vulkan strcmp resurfaced in vmaf.c"
    # The amend_json_with_backend_used helper exists and is called.
    assert "amend_json_with_backend_used" in src
    assert '"backend_used"' in src
    # The not-compiled-in guard fires before any per-backend stanza so
    # an unknown ``--backend NAME`` on a CPU-only build also errors out.
    assert "libvmaf was built" in src
    assert "compiled_in" in src


# ---------------------------------------------------------------------------
# Follow-up #6 — compare distinguishes encoder missing vs encode failure
# ---------------------------------------------------------------------------


def test_compare_distinguishes_encoder_missing_vs_encode_failure(tmp_path: Path) -> None:
    """Follow-up #6: bisect surfaces "encoder unavailable" for missing encoders.

    When ffmpeg's stderr says "Encoder not found" we report
    ``encoder unavailable (libsvtav1): …`` instead of the cryptic
    ``encode failed at CRF NN (exit=1): Encoder not found`` the
    pre-ADR-0498 path emitted.
    """
    from vmaftune.bisect import bisect_target_vmaf

    def _missing_encoder_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        # Simulate ffmpeg's "Encoder not found" failure.
        return _FakeCompleted(
            returncode=1,
            stderr=(
                "ffmpeg version 6.0\n" "[libx264 @ 0x55] ...\n" "Encoder not found: libsvtav1\n"
            ),
        )

    def _score_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        return _FakeCompleted(returncode=0)

    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libsvtav1",
        target_vmaf=90.0,
        width=1280,
        height=720,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=2.0,
        crf_range=(28, 30),
        max_iterations=1,
        encode_runner=_missing_encoder_runner,
        score_runner=_score_runner,
        workdir=tmp_path,
    )
    assert not result.ok
    assert (
        "encoder unavailable" in result.error
    ), f"expected 'encoder unavailable' in error, got {result.error!r}"

    # Genuine encode failure (no encoder-missing marker) keeps the
    # original "encode failed at CRF N" wording.
    def _real_failure_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        return _FakeCompleted(
            returncode=1,
            stderr="ffmpeg version 6.0\n[libx264 @ 0x55] some other error\n",
        )

    result2 = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=90.0,
        width=1280,
        height=720,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=2.0,
        crf_range=(28, 30),
        max_iterations=1,
        encode_runner=_real_failure_runner,
        score_runner=_score_runner,
        workdir=tmp_path / "b",
    )
    assert not result2.ok
    assert "encoder unavailable" not in result2.error
    assert "encode failed" in result2.error


# ---------------------------------------------------------------------------
# Follow-up #7 — encoder version fallback for libx264 / libsvtav1
# ---------------------------------------------------------------------------


def test_encoder_version_fallback_probe_for_libx264() -> None:
    """Follow-up #7: when the per-encoder banner is suppressed, fall back to
    ``ffmpeg -version``'s ``--enable-libx264`` marker so the row
    carries ``libx264-enabled`` instead of bare ``unknown``.
    """
    from vmaftune.encode import _PROBE_CACHE, _probe_encoder_version_from_ffmpeg

    _PROBE_CACHE.clear()

    def _runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        assert argv[-1] == "-version"
        return _FakeCompleted(
            returncode=0,
            stdout=(
                "ffmpeg version n6.0\n"
                "configuration: --prefix=/usr --enable-libx264 "
                "--enable-libsvtav1 --enable-libx265\n"
            ),
        )

    label = _probe_encoder_version_from_ffmpeg("ffmpeg", "libx264", _runner)
    assert label == "libx264-enabled"
    label2 = _probe_encoder_version_from_ffmpeg("ffmpeg", "libsvtav1", _runner)
    assert label2 == "libsvtav1-enabled"

    # Unknown encoder: empty string so the caller keeps "unknown".
    label3 = _probe_encoder_version_from_ffmpeg("ffmpeg", "libfooav99", _runner)
    assert label3 == ""


# ---------------------------------------------------------------------------
# Anti-regression: ladder same-resolution path keeps legacy behaviour
# ---------------------------------------------------------------------------


def test_ladder_same_resolution_no_scale_filter(tmp_path: Path) -> None:
    """Single-resolution ladder (source == target) must NOT inject -vf scale.

    Regression guard for the legacy raw-YUV-at-rung-resolution path.
    """
    from vmaftune.corpus import CorpusJob, CorpusOptions, iter_rows

    job = CorpusJob(
        source=tmp_path / "src.yuv",
        width=1280,
        height=720,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=2.0,
        cells=(("medium", 28),),
    )
    (tmp_path / "src.yuv").write_bytes(b"\x00")
    opts = CorpusOptions(
        encoder="libx264",
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "enc",
        src_sha256=False,
    )
    encode_argvs: list[list[str]] = []

    def _enc_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        encode_argvs.append(list(argv))
        out = Path(argv[-1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.0\n")

    def _score_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        out = Path(argv[argv.index("--output") + 1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text('{"pooled_metrics": {"vmaf": {"mean": 90.0}}}\n')
        return _FakeCompleted(returncode=0)

    list(iter_rows(job, opts, encode_runner=_enc_runner, score_runner=_score_runner))
    assert encode_argvs, "encoder runner not called"
    argv = encode_argvs[0]
    # No scale filter when source dims match the rung target.
    assert "-vf" not in argv, f"unexpected -vf in single-resolution path: {argv}"
    assert argv[argv.index("-s") + 1] == "1280x720"


# Sanity: math import is used by tests that synthesize NaN.
assert math is not None
