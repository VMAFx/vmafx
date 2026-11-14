# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Behaviour tests for the loopback hwaccel checker, using a stub ffmpeg."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

CHECKER = REPO / "ffmpeg-patches/test/loopback_hwaccel_check.py"

STUB = """#!/usr/bin/env python3
import os
import pathlib
import sys

args = sys.argv[1:]


def fail(text):
    sys.stderr.write(text + "\\n")
    raise SystemExit(1)


def value(flag):
    try:
        return args[args.index(flag) + 1]
    except (ValueError, IndexError):
        return ""


if "-version" in args:
    if os.environ.get("STUB_VERSION_FAIL"):
        fail("boom")
    print("ffmpeg version 9.0.2")
    raise SystemExit(0)

if "-dec" not in args and value("-c:v") == "libx264":
    if os.environ.get("STUB_ENCODE_FAIL"):
        fail("Unknown encoder 'libx264'")
    out = pathlib.Path(args[-1])
    out.write_bytes(b"h264")
    raise SystemExit(0)

if "-init_hw_device" in args:
    raise SystemExit(0 if os.environ.get("STUB_GPU") else 1)

if "-dec" in args and "-" not in args[: args.index("-dec")]:
    # FFmpeg's scheduler refuses a loopback decoder fed by its own filtergraph's output.
    fail("[SchFilterGraph] Transcoding graph has a cycle")

if value("-hwaccel").startswith("no-such"):
    fail("Unrecognized hwaccel for a loopback decoder: " + value("-hwaccel"))

if value("-hwaccel_output_format").startswith("no-such"):
    fail("Unrecognised hwaccel output format: " + value("-hwaccel_output_format"))

if value("-hwaccel_device").startswith("/no/such"):
    fail(os.environ.get("STUB_BAD_DEVICE_TEXT", "Device creation failed: -2."))

if "hwdownload" in value("-filter_complex"):
    fail(os.environ.get("STUB_HWDOWNLOAD_TEXT", "Impossible to convert between the formats"))

frames = os.environ.get("STUB_LOOPBACK_FRAMES", "6")
sys.stderr.write("frame=    6 fps=0.0 q=-1.0 size=N/A time=00:00:00.24 bitrate=N/A\\n")
sys.stderr.write(f"Output stream #1:0 (video): {frames} frames encoded; {frames} packets muxed\\n")
raise SystemExit(0)
"""


@unittest.skipIf(os.name == "nt", "the checker and its stub run on POSIX hosts")
class LoopbackHwaccelCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.stub = self.root / "ffmpeg"
        self.stub.write_text(STUB, encoding="utf-8")
        self.stub.chmod(0o755)
        yuv_dir = self.root / "yuv"
        yuv_dir.mkdir()
        (yuv_dir / "src01_hrc01_576x324.yuv").write_bytes(b"\0" * (576 * 324 * 3 // 2 * 6))
        self.yuv = yuv_dir
        self.summary = self.root / "summary.txt"

    def run_checker(
        self, ffmpeg: str | None = None, *extra: str, env: dict[str, str] | None = None
    ) -> object:
        command = [
            sys.executable,
            str(CHECKER),
            "--ffmpeg",
            ffmpeg or str(self.stub),
            "--yuv",
            str(self.yuv),
            "--summary-out",
            str(self.summary),
            *extra,
        ]
        environment = dict(os.environ)
        if env:
            environment.update(env)
        return run_command(
            command,
            allowed_executables=(sys.executable,),
            check=False,
            capture_output=True,
            text=True,
            timeout_seconds=30,
            env=environment,
        )

    def test_positive_software_path_reports_only_rows_that_ran(self) -> None:
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SKIP gpu branch", result.stdout)
        self.assertIn("software loopback round trip decodes 6 frames", result.stdout)
        self.assertNotIn("evidence=none", result.stdout)

    def test_version_failure_reports_no_evidence(self) -> None:
        result = self.run_checker(env={"STUB_VERSION_FAIL": "1"})
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL ffmpeg binary runs (-version)", result.stdout)
        self.assertIn("evidence=none", result.stdout)
        self.assertNotIn("loopback refuses an unrecognised", result.stdout)

    def test_missing_binary_is_a_fail_row_with_summary(self) -> None:
        result = self.run_checker("/nonexistent/ffmpeg-3351")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL ffmpeg binary runs (-version)", result.stdout)
        self.assertIn("No such file or directory", result.stdout)
        self.assertNotIn("Traceback", result.stdout + result.stderr)
        self.assertIn("evidence=none", self.summary.read_text(encoding="utf-8"))

    def test_gpu_skip_can_be_required(self) -> None:
        result = self.run_checker(None, "--require-gpu")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL gpu device of type cuda usable", result.stdout)

    def test_round_trip_counts_the_loopback_output_not_the_encode(self) -> None:
        result = self.run_checker(env={"STUB_LOOPBACK_FRAMES": "0"})
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL software loopback round trip decodes 6 frames", result.stdout)
        self.assertIn("frames=0", result.stdout)

    def test_hwdownload_needs_its_own_diagnostic(self) -> None:
        result = self.run_checker(env={"STUB_HWDOWNLOAD_TEXT": "Error opening output file"})
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL software loopback frames are refused by hwdownload", result.stdout)

    def test_missing_fixture_is_a_fail_row(self) -> None:
        (self.yuv / "src01_hrc01_576x324.yuv").unlink()
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL fixture present", result.stdout)
        self.assertIn("evidence=none", result.stdout)

    def test_failed_encode_is_a_fail_row(self) -> None:
        result = self.run_checker(env={"STUB_ENCODE_FAIL": "1"})
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL fixture encodes through libx264", result.stdout)
        self.assertNotIn("loopback refuses an unrecognised", result.stdout)

    def test_gpu_branch_runs_when_a_device_exists(self) -> None:
        result = self.run_checker(None, "--require-gpu", env={"STUB_GPU": "1"})
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("ok   gpu round trip (-hwaccel cuda)", result.stdout)
        self.assertIn("ok   loopback refuses a bogus -hwaccel_device", result.stdout)
        self.assertIn("gpu=cuda ok", result.stdout)

    def test_bogus_device_needs_the_device_diagnostic(self) -> None:
        env = {"STUB_GPU": "1", "STUB_BAD_DEVICE_TEXT": "Error opening output file"}
        result = self.run_checker(None, "--require-gpu", env=env)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL loopback refuses a bogus -hwaccel_device", result.stdout)
