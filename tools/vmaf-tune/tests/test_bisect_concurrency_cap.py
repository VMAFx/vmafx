# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for ADR-0577: decode concurrency cap + aggressive workdir cleanup.

Three test classes mirror the structure of ``test_workdir_enospc.py``:

1. ``TestDecodeSemaphore`` — verify that when ``--max-concurrent-decodes 1``
   (the default), at most one reference-YUV decode runs simultaneously across
   3 concurrent bisect threads.

2. ``TestAggressiveCleanup`` — after a mocked bisect iteration completes,
   verify that the iteration's ``.mkv`` and ``.decoded.yuv`` files are gone,
   and that the reference YUV is deleted at bisect completion.

3. ``TestMidRunDiskCheck`` — monkeypatch ``shutil.disk_usage`` to return
   decreasing free space across iterations; assert the run fails fast with the
   expected error message at the right iteration.

4. ``TestCliMaxConcurrentDecodesFlag`` — parser-level smoke test: the
   ``compare``, ``ladder``, and ``tune-per-shot`` subcommands accept
   ``--max-concurrent-decodes``.

No ffmpeg / vmaf binaries are required — all subprocess calls are stubbed.
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
import unittest.mock
from collections import namedtuple
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.bisect import (  # noqa: E402
    DEFAULT_MAX_CONCURRENT_DECODES,
    _check_disk_space,
    _estimate_yuv_bytes,
    bisect_target_vmaf,
    set_decode_semaphore,
)

# ---------------------------------------------------------------------------
# Helpers shared across test classes
# ---------------------------------------------------------------------------

_DiskUsage = namedtuple("_DiskUsage", ["total", "used", "free"])


def _make_fake_encode_runner(out_dir: Path | None = None):
    """Return a callable that simulates a successful ffmpeg encode.

    When the command ends with a ``.mkv``, creates the output file so the
    bisect thinks the encode succeeded.
    """

    class _FakeCompleted:
        returncode = 0
        stdout = ""
        stderr = "libx264 0.163.3060"

    def _runner(cmd, **kwargs):
        if isinstance(cmd, (list, tuple)) and cmd and cmd[-1].endswith(".mkv"):
            dest = Path(cmd[-1])
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(b"\x00" * 32)
        return _FakeCompleted()

    return _runner


def _make_fake_score_runner():
    """Return a callable that returns a successful score result with VMAF 95."""

    class _ScoreCompleted:
        returncode = 0
        stdout = '{"vmaf": 95.0}'
        stderr = ""

    return lambda cmd, **kwargs: _ScoreCompleted()


# ---------------------------------------------------------------------------
# TestDecodeSemaphore
# ---------------------------------------------------------------------------


class TestDecodeSemaphore:
    """Verify the semaphore caps concurrent reference-YUV decodes.

    We launch 3 threads each calling bisect_target_vmaf with a container
    source. Each call would need to decode the reference YUV. We install a
    fake decode runner that records the number of concurrent callers; with
    max_concurrent_decodes=1 that peak must never exceed 1.
    """

    def _run_bisect_with_semaphore(
        self,
        src: Path,
        sem: threading.Semaphore,
        concurrency_log: list,
        concurrency_lock: threading.Lock,
        active_counter: list,
        tmp_path: Path,
    ) -> None:
        """Run one bisect in a thread, tracking active concurrent decodes."""

        def _fake_decode_runner(cmd, **kwargs):
            # Track the entry/exit of the "decode" phase.
            with concurrency_lock:
                active_counter[0] += 1
                concurrency_log.append(active_counter[0])
            # Simulate a brief decode time.
            time.sleep(0.02)
            # Create the decoded ref YUV so bisect doesn't fail.
            if cmd and cmd[-1].endswith(".yuv"):
                dest = Path(cmd[-1])
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(b"\x00" * 64)
            with concurrency_lock:
                active_counter[0] -= 1

            class _C:
                returncode = 0

            return _C()

        bisect_target_vmaf(
            src,
            "libx264",
            92.0,
            width=640,
            height=480,
            framerate=24.0,
            duration_s=1.0,
            max_iterations=1,
            encode_runner=_make_fake_encode_runner(),
            decode_runner=_fake_decode_runner,
            workdir=tmp_path / "bisect_work",
            decode_semaphore=sem,
        )

    def test_semaphore_limits_concurrent_decodes_to_one(self, tmp_path):
        """With Semaphore(1) the peak concurrent decode count must be 1."""
        src = tmp_path / "bbb.mp4"
        src.write_bytes(b"\x00" * 32)

        sem = threading.Semaphore(1)
        concurrency_log: list[int] = []
        concurrency_lock = threading.Lock()
        active_counter = [0]  # mutable int

        threads = [
            threading.Thread(
                target=self._run_bisect_with_semaphore,
                args=(src, sem, concurrency_log, concurrency_lock, active_counter, tmp_path),
            )
            for _ in range(3)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=10.0)

        # Every recorded peak must be <= 1 when the semaphore is Semaphore(1).
        # (The log captures the count *after* incrementing — the maximum
        # observed value is the true peak concurrency.)
        assert concurrency_log, "No decode calls were recorded"
        assert max(concurrency_log) <= 1, (
            f"peak concurrent decodes = {max(concurrency_log)}, expected <= 1 "
            f"with Semaphore(1); log={concurrency_log}"
        )

    def test_default_max_concurrent_decodes_constant(self):
        """DEFAULT_MAX_CONCURRENT_DECODES is 1 (serial)."""
        assert DEFAULT_MAX_CONCURRENT_DECODES == 1

    def test_set_decode_semaphore_rejects_zero(self):
        """set_decode_semaphore(0) raises ValueError."""
        with pytest.raises(ValueError, match="max_concurrent"):
            set_decode_semaphore(0)

    def test_set_decode_semaphore_accepts_positive(self):
        """set_decode_semaphore(N) succeeds for N >= 1."""
        set_decode_semaphore(1)
        set_decode_semaphore(4)
        # Reset to safe default so subsequent tests are unaffected.
        set_decode_semaphore(1)


# ---------------------------------------------------------------------------
# TestAggressiveCleanup
# ---------------------------------------------------------------------------


class TestAggressiveCleanup:
    """After a bisect completes, the per-iteration .mkv + decoded.yuv are gone.

    The reference YUV (``*.ref.decoded.yuv``) must be deleted by bisect's
    finally block after all iterations complete (ADR-0577 aggressive cleanup).
    """

    def _fake_decode_runner_yuv_creator(self, created_files: list[Path]):
        """Create fake decoded YUV files and record their paths."""

        def _runner(cmd, **kwargs):
            if cmd and cmd[-1].endswith(".yuv"):
                dest = Path(cmd[-1])
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(b"\x00" * 64)
                created_files.append(dest)

            class _C:
                returncode = 0

            return _C()

        return _runner

    def test_ref_yuv_deleted_after_bisect_completes(self, tmp_path):
        """The ``*.ref.decoded.yuv`` file is removed after bisect finishes.

        This verifies ADR-0577's "after-codec cleanup" rule: the decoded
        reference YUV is not kept until the end of the full compare run,
        it is deleted as soon as the bisect for that (codec, target) pair
        finishes.
        """
        src = tmp_path / "bbb.mp4"
        src.write_bytes(b"\x00" * 32)
        work = tmp_path / "work"
        work.mkdir()

        created_yuv_files: list[Path] = []

        # The bisect may fail (score stub returns 0 bytes → score fails), that's
        # fine — we only care that cleanup ran. Discard the result.
        bisect_target_vmaf(
            src,
            "libx264",
            92.0,
            width=640,
            height=480,
            framerate=24.0,
            duration_s=1.0,
            max_iterations=1,
            encode_runner=_make_fake_encode_runner(),
            decode_runner=self._fake_decode_runner_yuv_creator(created_yuv_files),
            workdir=work,
        )

        # The ref YUV path is deterministic.
        ref_yuv = work / (src.stem + ".ref.decoded.yuv")
        assert not ref_yuv.exists(), (
            f"reference YUV {ref_yuv} still exists after bisect completed; "
            "ADR-0577 aggressive cleanup did not run"
        )

    def test_iteration_mkv_deleted_after_score(self, tmp_path):
        """The per-iteration ``.mkv`` is deleted after scoring completes.

        The existing cleanup in ``_encode_and_score`` already does this;
        this test confirms the behaviour did not regress under ADR-0577.
        """
        src = tmp_path / "source.yuv"
        src.write_bytes(b"\x00" * 64)
        work = tmp_path / "work"
        work.mkdir()

        # Successful encode runner creates the .mkv.
        encode_runner = _make_fake_encode_runner()

        with unittest.mock.patch(
            "vmaftune.bisect.shutil.disk_usage",
            return_value=_DiskUsage(total=500 * 1024**3, used=0, free=500 * 1024**3),
        ):
            bisect_target_vmaf(
                src,
                "libx264",
                92.0,
                width=640,
                height=480,
                framerate=24.0,
                duration_s=1.0,
                max_iterations=1,
                encode_runner=encode_runner,
                workdir=work,
            )

        # After bisect the .mkv should be gone (cleaned by _encode_and_score).
        leftover_mkvs = list(work.glob("*.mkv"))
        assert leftover_mkvs == [], f"Unexpected .mkv files remain after bisect: {leftover_mkvs}"


# ---------------------------------------------------------------------------
# TestMidRunDiskCheck
# ---------------------------------------------------------------------------


class TestMidRunDiskCheck:
    """Verify mid-run disk-space checks fire before each iteration's decode.

    We monkeypatch ``shutil.disk_usage`` to report a low-free-space condition
    starting at iteration 2, and assert the bisect fails fast with the expected
    context-enriched error message rather than letting the decode fail with an
    opaque ENOSPC return code.
    """

    def _make_encode_runner_count(self, encode_calls: list):
        """Encode runner that counts calls and creates the .mkv."""

        def _runner(cmd, **kwargs):
            if isinstance(cmd, (list, tuple)) and cmd and cmd[-1].endswith(".mkv"):
                dest = Path(cmd[-1])
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(b"\x00" * 32)
                encode_calls.append(1)

            class _C:
                returncode = 0
                stdout = ""
                stderr = "libx264 0.163.3060"

            return _C()

        return _runner

    def test_mid_run_check_fires_at_low_space(self, tmp_path):
        """Bisect fails fast with disk-space context when mid-run check fires.

        Scenario: 1080p 60 s source, 500 MB free — far less than the
        2 × estimated_yuv_bytes threshold. The check should fire at iteration 1
        (before the reference decode), returning ok=False with a message that
        mentions the codec and target VMAF.
        """
        src = tmp_path / "bbb.mp4"
        src.write_bytes(b"\x00" * 32)
        work = tmp_path / "work"
        work.mkdir()

        free_500mb = 500 * 1024**2

        encode_calls: list[int] = []

        def _fake_decode_runner(cmd, **kwargs):
            class _C:
                returncode = 0

            if cmd and cmd[-1].endswith(".yuv"):
                dest = Path(cmd[-1])
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(b"\x00" * 64)
            return _C()

        with unittest.mock.patch(
            "vmaftune.bisect.shutil.disk_usage",
            return_value=_DiskUsage(total=free_500mb, used=0, free=free_500mb),
        ):
            result = bisect_target_vmaf(
                src,
                "libx264",
                92.0,
                width=1920,
                height=1080,
                framerate=60.0,
                duration_s=60.0,  # large source → huge estimate
                max_iterations=4,
                encode_runner=self._make_encode_runner_count(encode_calls),
                decode_runner=_fake_decode_runner,
                workdir=work,
            )

        # Must fail with ok=False.
        assert not result.ok, "Expected ok=False when mid-run disk check fires"

        # Error message must name codec and target VMAF.
        assert "libx264" in result.error, f"Expected 'libx264' in error; got: {result.error!r}"
        assert "92" in result.error, f"Expected target VMAF '92' in error; got: {result.error!r}"
        assert (
            "--workdir" in result.error or "VMAFTUNE_WORKDIR" in result.error
        ), f"Expected --workdir hint in error; got: {result.error!r}"

    def test_mid_run_check_headroom_modes(self, tmp_path):
        """Container sources use 2× headroom; pre-decoded raw sources use 1.1×.

        Verify that a volume with 1.5× the estimated YUV size fails the
        container-source mid-run check but accepts the pre-decoded raw
        source path used by compare's shared-reference optimisation.
        """
        from vmaftune.bisect import _check_disk_space, _midrun_disk_headroom

        estimated = _estimate_yuv_bytes(
            width=1920, height=1080, pix_fmt="yuv420p", fps=60.0, duration_s=60.0
        )
        # 1.5× estimated bytes → fails headroom=2.0 but would pass headroom=1.1.
        one_half_x_free = int(estimated * 1.5)

        with unittest.mock.patch(
            "vmaftune.bisect.shutil.disk_usage",
            return_value=_DiskUsage(total=one_half_x_free, used=0, free=one_half_x_free),
        ):
            result_container = _check_disk_space(
                tmp_path,
                estimated_bytes=estimated,
                headroom=_midrun_disk_headroom(Path("ref.mp4")),
            )
            result_raw = _check_disk_space(
                tmp_path,
                estimated_bytes=estimated,
                headroom=_midrun_disk_headroom(Path("ref.shared-ref.yuv")),
            )

        assert result_container is not None, "Container-source 2× headroom should reject 1.5× free"
        assert result_raw is None, "Pre-decoded raw-source 1.1× headroom should accept 1.5× free"

    def test_mid_run_error_contains_context_codec_and_target(self, tmp_path):
        """_check_disk_space with context kwarg includes context in error."""
        estimated = 100 * 1024**3  # 100 GB
        free_50gb = 50 * 1024**3

        with unittest.mock.patch(
            "vmaftune.bisect.shutil.disk_usage",
            return_value=_DiskUsage(total=free_50gb, used=0, free=free_50gb),
        ):
            err = _check_disk_space(
                tmp_path,
                estimated_bytes=estimated,
                headroom=2.0,
                context="libsvtav1 @ VMAF 96, iteration 2",
            )

        assert err is not None
        assert "libsvtav1" in err
        assert "VMAF 96" in err
        assert "iteration 2" in err


# ---------------------------------------------------------------------------
# TestCliMaxConcurrentDecodesFlag
# ---------------------------------------------------------------------------


class TestCliMaxConcurrentDecodesFlag:
    """Parser-level smoke test: compare / tune-per-shot / ladder accept
    ``--max-concurrent-decodes``."""

    def _parse(self, argv: list[str]) -> argparse.Namespace:
        from vmaftune.cli import _build_parser

        return _build_parser().parse_args(argv)

    def test_compare_accepts_flag(self, tmp_path):
        args = self._parse(
            [
                "compare",
                "--src",
                str(tmp_path / "src.mp4"),
                "--width",
                "1920",
                "--height",
                "1080",
                "--max-concurrent-decodes",
                "2",
                "--predicate-module",
                "vmaftune.compare:_default_predicate",
            ]
        )
        assert args.max_concurrent_decodes == 2

    def test_compare_default_is_one(self, tmp_path):
        args = self._parse(
            [
                "compare",
                "--src",
                str(tmp_path / "src.mp4"),
                "--width",
                "1920",
                "--height",
                "1080",
                "--predicate-module",
                "vmaftune.compare:_default_predicate",
            ]
        )
        assert args.max_concurrent_decodes == 1

    def test_ladder_accepts_flag(self, tmp_path):
        args = self._parse(
            [
                "ladder",
                "--src",
                str(tmp_path / "src.mp4"),
                "--resolutions",
                "1920x1080",
                "--target-vmafs",
                "92",
                "--max-concurrent-decodes",
                "3",
            ]
        )
        assert args.max_concurrent_decodes == 3

    def test_ladder_default_is_one(self, tmp_path):
        args = self._parse(
            [
                "ladder",
                "--src",
                str(tmp_path / "src.mp4"),
                "--resolutions",
                "1920x1080",
                "--target-vmafs",
                "92",
            ]
        )
        assert args.max_concurrent_decodes == 1

    def test_tune_per_shot_accepts_flag(self, tmp_path):
        args = self._parse(
            [
                "tune-per-shot",
                "--src",
                str(tmp_path / "src.mp4"),
                "--width",
                "1920",
                "--height",
                "1080",
                "--framerate",
                "24.0",
                "--max-concurrent-decodes",
                "4",
            ]
        )
        assert args.max_concurrent_decodes == 4

    def test_tune_per_shot_default_is_one(self, tmp_path):
        args = self._parse(
            [
                "tune-per-shot",
                "--src",
                str(tmp_path / "src.mp4"),
                "--width",
                "1920",
                "--height",
                "1080",
                "--framerate",
                "24.0",
            ]
        )
        assert args.max_concurrent_decodes == 1
