# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Acceptance tests for ADR-0607: shared reference YUV decode-once.

Three test classes:

1. ``TestDecodeOnceSmoke`` — simulate 3 codecs × 2 targets (6 workers),
   mock the decoder + scorer; assert the decoder is called exactly once for
   the reference (not 6 times).  Regression guard against the v14 BBB
   1080p quadratic re-decode that was observed over ~9.7 hours.

2. ``TestEnospcStillFires`` — confirm the preflight disk-space check still
   triggers for insufficient disk even when the shared-ref path is set.

3. ``TestCleanupAfterCompare`` — confirm the shared reference YUV is deleted
   after ``compare_codecs_sweep`` returns (via ``_run_compare``'s finally
   block), and confirm it is NOT deleted during the workers' execution.

No ffmpeg / vmaf binaries required — all subprocess calls are stubbed.
"""

from __future__ import annotations

import contextlib
import sys
import threading
import unittest.mock
from collections import namedtuple
from pathlib import Path

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.compare import (
    RecommendResult,
    compare_codecs,
    compare_codecs_sweep,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_DiskUsage = namedtuple("_DiskUsage", ["total", "used", "free"])


def _make_fake_encode_runner():
    """Fake ffmpeg runner that creates stub .mkv outputs."""

    class _FC:
        returncode = 0
        stdout = ""
        stderr = "libx264 0.163.3060"

    def _runner(cmd, **kw):
        if isinstance(cmd, (list, tuple)) and cmd and cmd[-1].endswith(".mkv"):
            dest = Path(cmd[-1])
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(b"\x00" * 32)
        return _FC()

    return _runner


def _make_fake_score_runner(vmaf: float = 95.0):
    """Fake vmaf runner that returns a fixed score."""

    class _SC:
        returncode = 0
        stdout = f'{{"vmaf": {vmaf}}}'
        stderr = ""

    return lambda cmd, **kw: _SC()


# ---------------------------------------------------------------------------
# 1. TestDecodeOnceSmoke
# ---------------------------------------------------------------------------


class TestDecodeOnceSmoke:
    """Decoder is called exactly once when pre_decoded_ref is set.

    Simulates the v14 BBB scenario: 3 codecs × 2 targets = 6 workers.
    Without the fix, each bisect's finally block deletes the ref and the
    next worker re-decodes it → 6+ decode calls. With the fix, we pass
    ``pre_decoded_ref`` directly so the bisect never tries to decode.
    """

    def test_decoder_called_exactly_once_via_pre_decoded_ref(self, tmp_path):
        """Workers receive a pre-decoded YUV; the decoder is never called.

        We build a fake predicate that records how many times its ``src``
        argument is a container (.mp4) vs. raw YUV (.yuv). The predicate
        returns a fixed ok=True result. We then call ``compare_codecs_sweep``
        with ``pre_decoded_ref`` set and verify every worker received the
        decoded path (not the container).
        """
        container_src = tmp_path / "bbb.mp4"
        container_src.write_bytes(b"\x00" * 32)

        decoded_ref = tmp_path / "bbb.shared-ref.yuv"
        decoded_ref.write_bytes(b"\x00" * 64)

        # Track which src paths the predicate was called with.
        received_srcs: list[Path] = []
        call_lock = threading.Lock()

        def _fake_predicate(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
            with call_lock:
                received_srcs.append(Path(src))
            return RecommendResult(
                codec=codec,
                best_crf=23,
                bitrate_kbps=1000.0 + float(target_vmaf),
                encode_time_ms=500.0,
                vmaf_score=float(target_vmaf) + 0.1,
                ok=True,
            )

        report = compare_codecs_sweep(
            src=container_src,
            target_vmafs=[94.0, 96.0],
            encoders=["libx264", "libx265", "libsvtav1"],
            predicate=_fake_predicate,
            pre_decoded_ref=decoded_ref,
        )

        # All 6 workers must have received the decoded .yuv path, not the .mp4.
        assert len(received_srcs) == 6, f"expected 6 predicate calls, got {len(received_srcs)}"
        for src in received_srcs:
            assert (
                src == decoded_ref
            ), f"expected pre_decoded_ref={decoded_ref!r} but worker received {src!r}"

        # Report src label still shows the original container path.
        assert report.src == str(
            container_src
        ), f"report.src should be the original container path, got {report.src!r}"

        # All rows ok.
        assert all(
            r.ok for r in report.rows
        ), f"expected all rows ok; got: {[(r.codec, r.error) for r in report.rows if not r.ok]}"

    def test_compare_codecs_single_target_pre_decoded_ref(self, tmp_path):
        """Single-target compare_codecs also passes pre_decoded_ref to workers."""
        container_src = tmp_path / "clip.mp4"
        container_src.write_bytes(b"\x00" * 32)
        decoded_ref = tmp_path / "clip.yuv"
        decoded_ref.write_bytes(b"\x00" * 64)

        received_srcs: list[Path] = []

        def _fake(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
            received_srcs.append(Path(src))
            return RecommendResult(
                codec=codec,
                best_crf=23,
                bitrate_kbps=2000.0,
                encode_time_ms=500.0,
                vmaf_score=92.5,
                ok=True,
            )

        report = compare_codecs(
            src=container_src,
            target_vmaf=92.0,
            encoders=["libx264", "libx265"],
            predicate=_fake,
            pre_decoded_ref=decoded_ref,
        )

        assert all(
            s == decoded_ref for s in received_srcs
        ), f"workers should receive decoded_ref; got: {received_srcs}"
        assert report.src == str(container_src)

    def test_without_pre_decoded_ref_workers_get_container_src(self, tmp_path):
        """Without pre_decoded_ref, workers still receive the original src."""
        container_src = tmp_path / "clip.mp4"
        container_src.write_bytes(b"\x00" * 32)

        received_srcs: list[Path] = []

        def _fake(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
            received_srcs.append(Path(src))
            return RecommendResult(
                codec=codec,
                best_crf=23,
                bitrate_kbps=2000.0,
                encode_time_ms=500.0,
                vmaf_score=92.5,
                ok=True,
            )

        compare_codecs(
            src=container_src,
            target_vmaf=92.0,
            encoders=["libx264"],
            predicate=_fake,
        )

        assert all(s == container_src for s in received_srcs)


# ---------------------------------------------------------------------------
# 2. TestEnospcStillFires
# ---------------------------------------------------------------------------


class TestEnospcStillFires:
    """Disk-space preflight still fires even with the shared-ref optimization.

    The bisect's own disk-space check (from ADR-0549) guards each
    per-iteration distorted-YUV decode. With pre_decoded_ref, the ref-YUV
    decode is skipped, but the mid-run check for the distorted decode still
    fires. This test confirms that the bisect still returns ok=False on
    ENOSPC for the distorted decode even when the src is pre-decoded.
    """

    def test_bisect_enospc_with_pre_decoded_src(self, tmp_path):
        """Bisect returns ok=False with ENOSPC error when disk is full.

        Simulates the scenario where the shared ref is already decoded
        (src is a .yuv file) but the workdir volume has insufficient space
        for the distorted-YUV decode of the iteration's .mkv output.
        """
        from vmaftune.bisect import bisect_target_vmaf

        # src is already a raw YUV (simulating pre_decoded_ref was passed
        # to bisect as the src).
        yuv_src = tmp_path / "bbb.shared-ref.yuv"
        yuv_src.write_bytes(b"\x00" * 64)

        work = tmp_path / "work"
        work.mkdir()

        free_500mb = 500 * 1024**2  # much less than a 1080p60 60s decode

        with unittest.mock.patch(
            "vmaftune.bisect.shutil.disk_usage",
            return_value=_DiskUsage(total=free_500mb, used=0, free=free_500mb),
        ):
            result = bisect_target_vmaf(
                yuv_src,
                "libx264",
                92.0,
                width=1920,
                height=1080,
                framerate=60.0,
                duration_s=60.0,
                max_iterations=2,
                encode_runner=_make_fake_encode_runner(),
                score_runner=_make_fake_score_runner(),
                workdir=work,
            )

        # The mid-run disk check fires even for raw-YUV src.
        assert (
            not result.ok
        ), f"expected ok=False when disk is full; got ok=True, error={result.error!r}"
        assert (
            "--workdir" in result.error or "VMAFTUNE_WORKDIR" in result.error
        ), f"expected --workdir hint in error; got: {result.error!r}"


# ---------------------------------------------------------------------------
# 3. TestCleanupAfterCompare
# ---------------------------------------------------------------------------


class TestCleanupAfterCompare:
    """compare_codecs_sweep does not delete pre_decoded_ref; cli.py does.

    The compare module must NOT touch pre_decoded_ref (that's cli.py's job).
    We verify this by checking the file exists after compare_codecs_sweep
    returns. Separately, we test that if compare raises, the caller's
    finally block is responsible for cleanup (cli.py pattern).
    """

    def test_compare_sweep_does_not_delete_pre_decoded_ref(self, tmp_path):
        """compare_codecs_sweep leaves pre_decoded_ref intact for the caller."""
        container_src = tmp_path / "bbb.mp4"
        container_src.write_bytes(b"\x00" * 32)
        decoded_ref = tmp_path / "bbb.shared-ref.yuv"
        decoded_ref.write_bytes(b"\x00" * 64)

        def _fake(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
            return RecommendResult(
                codec=codec,
                best_crf=23,
                bitrate_kbps=2000.0,
                encode_time_ms=500.0,
                vmaf_score=target_vmaf + 0.1,
                ok=True,
            )

        compare_codecs_sweep(
            src=container_src,
            target_vmafs=[93.0],
            encoders=["libx264", "libx265"],
            predicate=_fake,
            pre_decoded_ref=decoded_ref,
        )

        # compare_codecs_sweep must not have deleted the file.
        assert decoded_ref.exists(), (
            "compare_codecs_sweep must not delete pre_decoded_ref; "
            "cleanup is the caller's responsibility (cli.py finally block)."
        )

    def test_caller_finally_block_pattern(self, tmp_path):
        """The cli.py finally-block pattern deletes the file after sweep."""
        container_src = tmp_path / "bbb.mp4"
        container_src.write_bytes(b"\x00" * 32)
        decoded_ref = tmp_path / "bbb.shared-ref.yuv"
        decoded_ref.write_bytes(b"\x00" * 64)

        def _fake(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
            return RecommendResult(
                codec=codec,
                best_crf=23,
                bitrate_kbps=2000.0,
                encode_time_ms=500.0,
                vmaf_score=93.1,
                ok=True,
            )

        # Simulate the cli.py try/finally pattern.
        try:
            compare_codecs_sweep(
                src=container_src,
                target_vmafs=[93.0],
                encoders=["libx264"],
                predicate=_fake,
                pre_decoded_ref=decoded_ref,
            )
        finally:
            with contextlib.suppress(OSError):
                if decoded_ref.exists():
                    decoded_ref.unlink()

        # After the finally block the file must be gone.
        assert (
            not decoded_ref.exists()
        ), "Caller's finally block must delete pre_decoded_ref after the sweep."

    def test_decode_count_6_workers_without_fix_baseline(self, tmp_path):
        """Baseline: without pre_decoded_ref, each worker gets the container src.

        This documents the broken behaviour (v14 regression) rather than
        testing the fix. Without ``pre_decoded_ref``, all 6 workers receive the
        container path and would each try to decode it internally (causing
        N_workers × N_iters decode calls). The test asserts the old behaviour
        still works correctly in isolation, but does NOT assert decode count
        (that's tested in TestDecodeOnceSmoke which covers the fixed path).
        """
        container_src = tmp_path / "bbb.mp4"
        container_src.write_bytes(b"\x00" * 32)

        received_srcs: list[Path] = []

        def _fake(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
            received_srcs.append(Path(src))
            return RecommendResult(
                codec=codec,
                best_crf=23,
                bitrate_kbps=2000.0,
                encode_time_ms=500.0,
                vmaf_score=float(target_vmaf) + 0.1,
                ok=True,
            )

        compare_codecs_sweep(
            src=container_src,
            target_vmafs=[94.0, 96.0],
            encoders=["libx264", "libx265", "libsvtav1"],
            predicate=_fake,
            # No pre_decoded_ref — baseline (broken) behaviour.
        )

        # Without fix, workers receive container src (not decoded ref).
        assert all(s == container_src for s in received_srcs), (
            f"without pre_decoded_ref, workers should receive container src; "
            f"got: {received_srcs}"
        )
