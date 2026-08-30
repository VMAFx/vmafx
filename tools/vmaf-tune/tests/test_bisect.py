# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Phase B target-VMAF bisect — unit tests.

Every test stubs the encode and score subprocess seams so neither
ffmpeg nor vmaf is actually invoked. The synthetic VMAF curve is
monotone-decreasing in CRF for the realistic cases; one test
deliberately violates monotonicity to exercise the safety bail-out.
"""

from __future__ import annotations

import math
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import bisect as bisect_mod
from vmaftune.bisect import (
    BisectResult,
    bisect_target_vmaf,
    make_bisect_predicate,
)
from vmaftune.compare import RecommendResult, compare_codecs

# ----------------------------------------------------------------------
# Synthetic encode / score harness
# ----------------------------------------------------------------------


@dataclass
class _FakeCompleted:
    """Minimal stand-in for :class:`subprocess.CompletedProcess`."""

    returncode: int = 0
    stdout: str = ""
    stderr: str = "ffmpeg version 0-test\nx264 - core 164\nVMAF version 3.0.0-test\n"


def _make_runners(
    crf_to_vmaf: Callable[[int], float],
    *,
    bytes_per_crf: Callable[[int], int] | None = None,
    crf_to_score_rc: Callable[[int], int] | None = None,
    crf_to_encode_rc: Callable[[int], int] | None = None,
) -> tuple[
    Callable[..., _FakeCompleted],
    Callable[..., _FakeCompleted],
    list[dict[str, Any]],
]:
    """Construct stubbed (encode_runner, score_runner, log) trio.

    The score runner intercepts the libvmaf JSON-output path from the
    argv ``--output PATH`` flag, materialises a JSON file at that
    path with the synthetic VMAF score, and returns a successful
    CompletedProcess. The encode runner just touches the output file
    so the size lookup in :func:`run_encode` returns a non-zero
    bitrate.
    """
    bytes_fn = bytes_per_crf or (lambda crf: max(1, 10_000_000 - crf * 100_000))
    rc_score = crf_to_score_rc or (lambda crf: 0)
    rc_encode = crf_to_encode_rc or (lambda crf: 0)
    log: list[dict[str, Any]] = []

    def _crf_from_argv(argv: list[str]) -> int:
        if "-crf" in argv:
            return int(argv[argv.index("-crf") + 1])
        if "-cq" in argv:
            return int(argv[argv.index("-cq") + 1])
        return -1

    def _encode_runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        crf = _crf_from_argv(argv)
        out_path = Path(argv[-1])
        rc = rc_encode(crf)
        if rc == 0:
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_bytes(b"\x00" * bytes_fn(crf))
        # Distinguish real encode invocations (which carry an explicit
        # ``-crf`` / ``-cq`` quality flag) from the post-encode decode
        # invocation introduced by the Bug #3 fix (a vanilla
        # ``ffmpeg -i <mkv> ... rawvideo`` that has no quality flag).
        # Existing assertions count encode invocations only.
        kind = "decode" if crf < 0 else "encode"
        log.append({"kind": kind, "crf": crf, "argv": list(argv), "rc": rc})
        return _FakeCompleted(returncode=rc)

    def _score_runner(argv: list[str], **_kwargs: Any) -> _FakeCompleted:
        # Score runner is invoked with the libvmaf CLI argv. We extract
        # the -crf-equivalent from the *distorted* path the encode
        # runner wrote (filename embeds crf=...) and emit a vmaf JSON.
        # The decoded sidecar path used by the Bug #3 fix carries an
        # extra ``.decoded`` stem segment we strip before parsing.
        if "--distorted" in argv:
            distorted = Path(argv[argv.index("--distorted") + 1])
            stem = distorted.stem
            stem = stem.removesuffix(".decoded")
            stem_parts = stem.split("_")
            try:
                crf = int(stem_parts[-1])
            except ValueError:
                crf = -1
        else:
            crf = -1
        rc = rc_score(crf)
        if "--output" in argv and rc == 0:
            out = Path(argv[argv.index("--output") + 1])
            out.parent.mkdir(parents=True, exist_ok=True)
            vmaf = crf_to_vmaf(crf) if crf >= 0 else float("nan")
            out.write_text(
                '{"pooled_metrics": {"vmaf": {"mean": ' + repr(vmaf) + "}}}\n",
                encoding="utf-8",
            )
        log.append({"kind": "score", "crf": crf, "argv": list(argv), "rc": rc})
        return _FakeCompleted(returncode=rc)

    return _encode_runner, _score_runner, log


def _kwargs(**override: Any) -> dict[str, Any]:
    base = {
        "width": 1280,
        "height": 720,
        "pix_fmt": "yuv420p",
        "framerate": 24.0,
        "duration_s": 10.0,
    }
    base.update(override)
    return base


# ----------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------


def test_monotone_curve_converges_to_correct_crf():
    # Curve: VMAF(crf) = 100 - crf, monotone-decreasing. Target=80
    # means the largest CRF clearing the floor is 20.
    enc, sc, log = _make_runners(lambda c: 100.0 - float(c))
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(0, 51),
        max_iterations=8,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is True, result.error
    assert result.best_crf == 20
    assert math.isclose(result.measured_vmaf, 80.0, abs_tol=1e-6)
    assert result.n_iterations >= 1
    # We never exceeded max_iterations.
    encode_calls = [e for e in log if e["kind"] == "encode"]
    assert 1 <= len(encode_calls) <= 8


def test_unreachable_target_returns_ok_false_with_error():
    # Curve maxes out at 70 (saturated codec); target 95 cannot be hit.
    enc, sc, _log = _make_runners(lambda c: 70.0 - 0.1 * c)
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=95.0,
        crf_range=(0, 51),
        max_iterations=8,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is False
    assert "unreachable" in result.error
    assert result.best_crf == -1


def test_target_met_at_every_crf_returns_max_crf():
    # Curve always above target — bisect should converge to the top of
    # the search window (max compression while still meeting quality).
    enc, sc, _log = _make_runners(lambda c: 99.0)
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(0, 51),
        max_iterations=8,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is True
    assert result.best_crf == 51


def test_crf_range_defaults_to_encoder_absolute_range():
    # ADR-0538: the default search window is the encoder's absolute
    # CRF range (libx264 -> 0..51) rather than the adapter's
    # perceptually-informative ``quality_range``. A constant-VMAF
    # curve picks the top of whatever range we bisect over;
    # asserting it equals the encoder's absolute hi (51) confirms
    # the new default binding.
    enc, sc, _log = _make_runners(lambda c: 99.0)
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is True
    # libx264 absolute range hi = 51 (see _ABSOLUTE_CRF_RANGE_BY_NAME).
    assert result.best_crf == 51


def test_premium_target_libx264_target_96_returns_ok_true():
    """ADR-0538: bisect to VMAF >= 96 on libx264 must succeed.

    Pre-ADR-0538 the bisect defaulted to the adapter's
    ``quality_range`` (libx264 = (0, 51), no change) but for codecs
    whose informative window opens above CRF 0 (libx265 = (15, 40),
    libsvtav1 = (20, 50)) the high-VMAF targets were unreachable.
    This test pins the libx264 happy path against a curve that
    requires CRF <= 12 to clear VMAF 96 — well inside both the old
    and the new defaults — and asserts ok=True with achieved >= 95.5
    (the user-stated SLA from the PR brief).
    """
    enc, sc, _log = _make_runners(lambda c: 100.0 - 0.4 * float(c))
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=96.0,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is True, result.error
    assert result.measured_vmaf >= 95.5, (result.best_crf, result.measured_vmaf)


def test_premium_target_libx265_target_96_uses_absolute_range():
    """ADR-0538: libx265's informative range (15, 40) is too tight.

    Without the ADR-0538 default the bisect would search (15, 40)
    and reject CRF <= 14 via ``adapter.validate``. With ADR-0538 it
    searches (0, 51), reaches the low-CRF region, and clears
    VMAF 96. Regression guard for the supersession rationale.
    """
    enc, sc, _log = _make_runners(lambda c: 100.0 - 0.4 * float(c))
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx265",
        target_vmaf=96.0,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is True, result.error
    assert result.measured_vmaf >= 95.5


def test_premium_target_libsvtav1_target_97_uses_absolute_range():
    """ADR-0538: libsvtav1's informative range (20, 50) is too tight.

    AV1 saturates faster than H.264 in absolute VMAF terms so the
    synthetic curve here only needs CRF <= ~7 to clear VMAF 97. The
    pre-ADR-0538 default would have refused CRF <= 19 outright via
    the adapter validator; this regression test pins the fix.
    """
    enc, sc, _log = _make_runners(lambda c: 100.0 - 0.45 * float(c))
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libsvtav1",
        target_vmaf=97.0,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is True, result.error
    assert result.measured_vmaf >= 96.5


def test_max_iterations_4_halts_early():
    enc, sc, log = _make_runners(lambda c: 100.0 - c)
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(0, 51),
        max_iterations=4,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    encode_calls = [e for e in log if e["kind"] == "encode"]
    assert len(encode_calls) <= 4
    assert result.n_iterations <= 4
    # Best-so-far must reflect a CRF whose VMAF actually clears target.
    if result.ok:
        assert result.measured_vmaf >= 80.0


def test_no_real_subprocess_invoked(monkeypatch):
    # If the bisect ever falls back to the real subprocess.run, this
    # poison value would surface as a TypeError.
    poisoned = object()
    monkeypatch.setattr("vmaftune.encode.subprocess.run", poisoned, raising=True)
    monkeypatch.setattr("vmaftune.score.subprocess.run", poisoned, raising=True)

    enc, sc, _log = _make_runners(lambda c: 100.0 - c)
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=85.0,
        crf_range=(10, 30),
        max_iterations=6,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is True


def test_sample_clip_aligns_encode_and_score_windows():
    enc, sc, log = _make_runners(lambda c: 100.0 - c, bytes_per_crf=lambda _c: 4_000_000)
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(20, 20),
        max_iterations=1,
        sample_clip_seconds=4.0,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(duration_s=10.0, framerate=24.0),
    )
    assert result.ok is True
    assert result.bitrate_kbps == 8000.0
    encode_argv = next(entry["argv"] for entry in log if entry["kind"] == "encode")
    score_argv = next(entry["argv"] for entry in log if entry["kind"] == "score")
    assert encode_argv[encode_argv.index("-ss") + 1] == "3.0"
    assert encode_argv[encode_argv.index("-t") + 1] == "4.0"
    assert score_argv[score_argv.index("--frame_skip_ref") + 1] == "72"
    assert score_argv[score_argv.index("--frame_cnt") + 1] == "96"


def test_predicate_adapter_plugs_into_compare_codecs():
    enc, sc, _log = _make_runners(lambda c: 100.0 - c)
    predicate = make_bisect_predicate(
        target_vmaf=85.0,
        crf_range=(0, 40),
        max_iterations=6,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    report = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=85.0,
        encoders=("libx264",),
        predicate=predicate,
        parallel=False,
    )
    assert len(report.rows) == 1
    row: RecommendResult = report.rows[0]
    assert row.ok is True, row.error
    assert row.codec == "libx264"
    assert row.best_crf == 15
    assert math.isclose(row.vmaf_score, 85.0, abs_tol=1e-6)


def test_unknown_codec_returns_failure():
    enc, sc, _log = _make_runners(lambda c: 100.0 - c)
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libfoobar-nonexistent",
        target_vmaf=80.0,
        crf_range=(0, 51),
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is False
    assert "unknown codec" in result.error


def test_invalid_crf_range_returns_failure():
    enc, sc, _log = _make_runners(lambda c: 100.0 - c)
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(40, 10),
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is False
    assert "invalid crf_range" in result.error


def test_encode_failure_propagates_clean_error():
    # Encode fails at every CRF — we should bail with the encode error,
    # not loop forever.
    enc, sc, _log = _make_runners(
        lambda c: 100.0 - c,
        crf_to_encode_rc=lambda c: 1,
    )
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(0, 51),
        encode_runner=enc,
        score_runner=sc,
        max_iterations=4,
        **_kwargs(),
    )
    assert result.ok is False
    assert "encode failed" in result.error


def test_monotonicity_violation_aborts_with_clear_error():
    # Pathological curve: VMAF rises with CRF (clearly an instrumented
    # codec / corrupt content). We must bail rather than weaken to a
    # different search strategy.
    def curve(crf: int) -> float:
        # Increasing in CRF → violates the monotone-decreasing contract.
        return 50.0 + float(crf)

    enc, sc, _log = _make_runners(curve)
    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(0, 51),
        max_iterations=8,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    assert result.ok is False
    assert "monotonicity" in result.error


def test_to_recommend_result_projection_keeps_fields():
    res = BisectResult(
        codec="libx264",
        best_crf=22,
        measured_vmaf=88.5,
        bitrate_kbps=2400.0,
        encode_time_ms=1234.0,
        n_iterations=4,
        encoder_version="libx264-164",
    )
    rr = res.to_recommend_result()
    assert rr.codec == "libx264"
    assert rr.best_crf == 22
    assert rr.bitrate_kbps == 2400.0
    assert rr.encode_time_ms == 1234.0
    assert rr.vmaf_score == 88.5
    assert rr.encoder_version == "libx264-164"
    assert rr.ok is True


def test_predicate_with_multiple_codecs():
    # Two codecs with different curves — predicate dispatches per-codec
    # via the adapter registry; bisect honours each codec's range.
    def curve(crf: int) -> float:
        return 100.0 - float(crf)

    enc, sc, _log = _make_runners(curve)
    predicate = make_bisect_predicate(
        target_vmaf=80.0,
        crf_range=(0, 40),
        max_iterations=6,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(),
    )
    report = compare_codecs(
        src=Path("ref.yuv"),
        target_vmaf=80.0,
        encoders=("libx264", "libx265"),
        predicate=predicate,
        parallel=False,
    )
    rows_by_codec = {r.codec: r for r in report.rows}
    for codec_name in ("libx264", "libx265"):
        assert rows_by_codec[codec_name].ok is True
        assert rows_by_codec[codec_name].best_crf == 20


def test_predicate_forwards_sample_clip_seconds():
    enc, sc, log = _make_runners(lambda c: 100.0 - c)
    predicate = make_bisect_predicate(
        target_vmaf=80.0,
        crf_range=(20, 20),
        max_iterations=1,
        sample_clip_seconds=2.0,
        encode_runner=enc,
        score_runner=sc,
        **_kwargs(duration_s=8.0, framerate=30.0),
    )
    row = predicate("libx264", Path("ref.yuv"), 80.0)
    assert row.ok is True
    encode_argv = next(entry["argv"] for entry in log if entry["kind"] == "encode")
    score_argv = next(entry["argv"] for entry in log if entry["kind"] == "score")
    assert encode_argv[encode_argv.index("-ss") + 1] == "3.0"
    assert encode_argv[encode_argv.index("-t") + 1] == "2.0"
    assert score_argv[score_argv.index("--frame_skip_ref") + 1] == "90"
    assert score_argv[score_argv.index("--frame_cnt") + 1] == "60"


def test_module_exports_match_public_surface():
    public = set(bisect_mod.__all__)
    # ADR-0530 added ``BisectSample`` to surface the per-iteration probes
    # consumed by the compare-sweep rate-quality chart.
    # ADR-0577 added ``set_decode_semaphore`` + ``DEFAULT_MAX_CONCURRENT_DECODES``
    # for the decode-concurrency cap (fixes BBB v13 ENOSPC).
    assert public == {
        "BisectResult",
        "BisectSample",
        "DEFAULT_MAX_CONCURRENT_DECODES",
        "bisect_target_vmaf",
        "make_bisect_predicate",
        "set_decode_semaphore",
    }
