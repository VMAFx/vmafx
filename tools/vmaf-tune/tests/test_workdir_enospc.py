# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for ADR-0549 workdir relocation + disk-space preflight.

These tests verify that:
1. ``_check_disk_space`` fires the human-readable diagnostic (including
   the --workdir hint) when shutil.disk_usage reports insufficient free
   space — instead of a silent rc=228 (ENOSPC) from ffmpeg.
2. ``_estimate_yuv_bytes`` returns a plausible upper bound for common
   pix_fmts and typical video dimensions.
3. ``_workdir_parent`` respects the ``VMAFTUNE_WORKDIR`` env variable.
4. ``bisect_target_vmaf`` propagates the disk-space error through its
   normal ``BisectResult(ok=False, error=...)`` failure path so callers
   see a clean structured error rather than rc=228.
5. The ``compare --workdir`` CLI flag is accepted and passed to the
   bisect backend (parser-level smoke test; no real encode needed).

No ffmpeg / vmaf binaries are required — all subprocess calls are
stubbed.
"""

from __future__ import annotations

import argparse
import sys
import unittest.mock
from collections import namedtuple
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.bisect import (
    _check_disk_space,
    _estimate_yuv_bytes,
    _workdir_parent,
    bisect_target_vmaf,
)

# ---------------------------------------------------------------------------
# _estimate_yuv_bytes
# ---------------------------------------------------------------------------


class TestEstimateYuvBytes:
    def test_yuv420p_1080p60_10s(self):
        """1080p60 yuv420p 10 s should be in the ~895 MB ballpark."""
        est = _estimate_yuv_bytes(
            width=1920, height=1080, pix_fmt="yuv420p", fps=60.0, duration_s=10.0
        )
        # 1920 * 1080 * 1.5 * 600 frames = 1_866_240_000 bytes ≈ 1.74 GB
        # We expect the estimate to be >= the true size (conservative).
        assert est >= 1_866_240_000
        # Sanity cap: should not be more than 3x the true size.
        assert est <= 3 * 1_866_240_000

    def test_yuv420p_bbb_full_source(self):
        """634 s 1080p60 BBB raw YUV must estimate above 100 GB."""
        est = _estimate_yuv_bytes(
            width=1920, height=1080, pix_fmt="yuv420p", fps=60.0, duration_s=634.0
        )
        gb_100 = 100 * 1024**3
        assert est > gb_100, f"expected > 100 GB, got {est / 1024**3:.1f} GB"

    def test_zero_duration_gives_one_frame(self):
        est = _estimate_yuv_bytes(
            width=640, height=480, pix_fmt="yuv420p", fps=24.0, duration_s=0.0
        )
        # At least 1 frame
        assert est >= int(640 * 480 * 1.5)

    def test_10bit_doubles_roughly(self):
        est8 = _estimate_yuv_bytes(
            width=1920, height=1080, pix_fmt="yuv420p", fps=24.0, duration_s=5.0
        )
        est10 = _estimate_yuv_bytes(
            width=1920, height=1080, pix_fmt="yuv420p10le", fps=24.0, duration_s=5.0
        )
        assert est10 == pytest.approx(est8 * 2.0, rel=0.01)

    def test_unknown_pix_fmt_falls_back_to_420p(self):
        est_known = _estimate_yuv_bytes(
            width=640, height=480, pix_fmt="yuv420p", fps=24.0, duration_s=1.0
        )
        est_unknown = _estimate_yuv_bytes(
            width=640, height=480, pix_fmt="nv12_exotic", fps=24.0, duration_s=1.0
        )
        # Unknown format uses the default 1.5 bpp floor — same as yuv420p.
        assert est_unknown == est_known


# ---------------------------------------------------------------------------
# _check_disk_space
# ---------------------------------------------------------------------------

_DiskUsage = namedtuple("_DiskUsage", ["total", "used", "free"])


class TestCheckDiskSpace:
    def test_sufficient_space_returns_none(self, tmp_path):
        with unittest.mock.patch(
            "vmaftune.bisect.shutil.disk_usage",
            return_value=_DiskUsage(total=200 * 1024**3, used=0, free=200 * 1024**3),
        ):
            result = _check_disk_space(tmp_path, estimated_bytes=1 * 1024**3)
        assert result is None

    def test_insufficient_space_returns_error_string(self, tmp_path):
        """Simulates the BBB ENOSPC scenario: 8 GB free, 118 GB needed."""
        free_8gb = 8 * 1024**3
        needed = 118 * 1024**3
        with unittest.mock.patch(
            "vmaftune.bisect.shutil.disk_usage",
            return_value=_DiskUsage(total=200 * 1024**3, used=0, free=free_8gb),
        ):
            result = _check_disk_space(tmp_path, estimated_bytes=needed)
        assert result is not None
        # Must mention the diagnostic hint
        assert "--workdir" in result
        assert "VMAFTUNE_WORKDIR" in result
        # Must mention GB figures
        assert "GB" in result

    def test_disk_usage_oserror_returns_none(self, tmp_path):
        """If disk_usage raises, we fall through so real decode can attempt."""
        with unittest.mock.patch(
            "vmaftune.bisect.shutil.disk_usage", side_effect=OSError("permission denied")
        ):
            result = _check_disk_space(tmp_path, estimated_bytes=1 * 1024**3)
        assert result is None

    def test_exact_boundary_just_enough(self, tmp_path):
        """Exactly enough space (== required * headroom floor) should pass."""
        import math

        est = 100 * 1024**3
        # required = ceil(est * 1.1) — exactly what _check_disk_space computes
        required = math.ceil(est * 1.1)
        with unittest.mock.patch(
            "vmaftune.bisect.shutil.disk_usage",
            return_value=_DiskUsage(total=required, used=0, free=required),
        ):
            result = _check_disk_space(tmp_path, estimated_bytes=est)
        assert result is None


# ---------------------------------------------------------------------------
# _workdir_parent
# ---------------------------------------------------------------------------


class TestWorkdirParent:
    def test_returns_none_without_env(self, monkeypatch):
        monkeypatch.delenv("VMAFTUNE_WORKDIR", raising=False)
        assert _workdir_parent() is None

    def test_returns_path_from_env(self, tmp_path, monkeypatch):
        monkeypatch.setenv("VMAFTUNE_WORKDIR", str(tmp_path))
        result = _workdir_parent()
        assert result == tmp_path

    def test_empty_env_returns_none(self, monkeypatch):
        monkeypatch.setenv("VMAFTUNE_WORKDIR", "   ")
        assert _workdir_parent() is None


# ---------------------------------------------------------------------------
# bisect_target_vmaf — disk-space preflight integration
# ---------------------------------------------------------------------------


class TestBisectDiskSpacePreflight:
    """Verify that the disk-space check fires before the reference decode.

    We stub out:
    - ``shutil.disk_usage`` → tiny free space
    - The encode runner → fake success (so the encode doesn't fail first)
    - The ffmpeg decode runner → should NOT be called (check fires first)

    The bisect must return ``ok=False`` with an error string containing
    the --workdir hint rather than an opaque rc=228.
    """

    def _make_fake_encode_runner(self):
        """Return a callable that simulates a successful ffmpeg encode."""

        class _FakeCompleted:
            returncode = 0
            stdout = ""
            stderr = "libx264 0.163.3060"

        def _runner(cmd, **kwargs):
            # If encoding, create the output file so the bisect thinks it worked.
            if "-c:v" in cmd and cmd[-1].endswith(".mkv"):
                Path(cmd[-1]).parent.mkdir(parents=True, exist_ok=True)
                Path(cmd[-1]).write_bytes(b"\x00" * 16)
            return _FakeCompleted()

        return _runner

    def test_enospc_returns_structured_error(self, tmp_path):
        """rc=228 scenario -> structured error instead of opaque failure.

        A 1080p60 634 s BBB source decoded to raw YUV420p is ~118 GB.
        The dev-mcp container's /tmp is an 8 GB tmpfs.  Simulate that
        by setting free space to 500 MB and asking for a 600-second
        source decode -- the estimate will be >> 500 MB, triggering the
        preflight before ffmpeg is invoked.
        """
        # Source container (mp4 suffix triggers the container decode path)
        src = tmp_path / "bbb.mp4"
        src.write_bytes(b"\x00" * 32)

        # 500 MB free -- far less than 600 s @ 1080p60 YUV420p (~112 GB)
        free_500mb = 500 * 1024**2
        decode_was_called = []

        def _fake_decode_runner(cmd, **kwargs):
            decode_was_called.append(cmd)

            class _C:
                returncode = 228  # ENOSPC

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
                duration_s=600.0,  # 10-minute source -> huge raw YUV
                encode_runner=self._make_fake_encode_runner(),
                decode_runner=_fake_decode_runner,
                workdir=tmp_path / "work",
            )

        assert not result.ok, "expected ok=False on ENOSPC"
        assert (
            "--workdir" in result.error or "VMAFTUNE_WORKDIR" in result.error
        ), f"error should contain --workdir hint; got: {result.error!r}"
        # The ffmpeg decode should NOT have been called -- the preflight fires first.
        assert (
            len(decode_was_called) == 0
        ), "decode runner should not be called when preflight catches ENOSPC"


# ---------------------------------------------------------------------------
# CLI parser — --workdir flag acceptance
# ---------------------------------------------------------------------------


class TestCliWorkdirFlag:
    """Parser-level smoke test: compare / tune-per-shot / ladder accept --workdir."""

    def _parse(self, argv: list[str]) -> argparse.Namespace:
        from vmaftune.cli import _build_parser

        return _build_parser().parse_args(argv)

    def test_compare_workdir_accepted(self, tmp_path):
        args = self._parse(
            [
                "compare",
                "--src",
                str(tmp_path / "src.mp4"),
                "--width",
                "1920",
                "--height",
                "1080",
                "--workdir",
                str(tmp_path / "work"),
                "--predicate-module",
                "vmaftune.compare:_default_predicate",
            ]
        )
        assert args.workdir == tmp_path / "work"

    def test_compare_workdir_default_is_none(self, tmp_path):
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
        assert args.workdir is None

    def test_ladder_workdir_accepted(self, tmp_path):
        args = self._parse(
            [
                "ladder",
                "--src",
                str(tmp_path / "src.mp4"),
                "--resolutions",
                "1920x1080",
                "--target-vmafs",
                "92",
                "--workdir",
                str(tmp_path / "work"),
            ]
        )
        assert args.workdir == tmp_path / "work"

    def test_tune_per_shot_workdir_accepted(self, tmp_path):
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
                "--workdir",
                str(tmp_path / "work"),
            ]
        )
        assert args.workdir == tmp_path / "work"
