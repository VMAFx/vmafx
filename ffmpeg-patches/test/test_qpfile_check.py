# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Behaviour tests for qpfile_check.py --require-encoders."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

CHECKER = REPO / "ffmpeg-patches/test/qpfile_check.py"

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


def qpfile_offsets(path):
    text = pathlib.Path(path).read_text(encoding="utf-8")
    rows = [line.split() for line in text.splitlines()[1:] if line.strip()]
    return rows[0] if rows else []


if "-encoders" in args:
    for name in os.environ.get(
        "STUB_ENCODERS", "libx264,libaom-av1,libsvtav1"
    ).split(","):
        print(f" V..... {name}              fake encoder")
    raise SystemExit(0)

codec = value("-c:v")
if codec == "libx264":
    if "aq-mode=0" in args:
        fail("aq-mode=0 is incompatible with the qpfile offsets")
    qp = value("-qpfile")
    if qp and len(qpfile_offsets(qp)) > 36:
        fail("the qpfile grid does not match the macroblocks")
    out = pathlib.Path(args[-1])
    if qp:
        text = pathlib.Path(qp).read_text(encoding="utf-8")
        size = 300 if "-12" in text else 100 if "12" in text else 200
    else:
        size = 200
    out.write_bytes(b"x" * size)
    sys.stderr.write("libx264: qpfile=" + qp + "\\n")
    raise SystemExit(0)

if codec == "libsvtav1":
    pathlib.Path(args[-1]).write_bytes(b"x" * 20)
    sys.stderr.write("libsvtav1: qpfile=" + value("-qpfile") + "\\n")
    raise SystemExit(0)

if codec == "libaom-av1":
    if "-usage" not in args or value("-cpu-used") == "6":
        fail("needs -usage realtime and -cpu-used 7 or 8")
    pathlib.Path(args[-1]).write_bytes(b"x" * 20)
    sys.stderr.write("libaom-av1: qpfile=" + value("-qpfile") + "\\n")
    raise SystemExit(0)

fail("unexpected codec " + codec)
"""


@unittest.skipIf(os.name == "nt", "the checker and its stub run on POSIX hosts")
class QpfileStrictModeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.stub = self.root / "ffmpeg"
        self.stub.write_text(STUB, encoding="utf-8")
        self.stub.chmod(0o755)
        self.yuv = self.root / "yuv"
        self.yuv.mkdir()
        (self.yuv / "src01_hrc01_576x324.yuv").write_bytes(b"\0" * (576 * 324 * 3 // 2 * 6))

    def run_checker(self, ffmpeg: str | None = None, env: dict[str, str] | None = None) -> object:
        environment = dict(os.environ)
        if env:
            environment.update(env)
        return run_command(
            [
                sys.executable,
                str(CHECKER),
                "--ffmpeg",
                ffmpeg or str(self.stub),
                "--yuv",
                str(self.yuv),
                "--require-encoders",
            ],
            allowed_executables=(sys.executable,),
            check=False,
            capture_output=True,
            text=True,
            timeout_seconds=30,
            env=environment,
        )

    def test_strict_positive_path(self) -> None:
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ok   x264 applies the offsets", result.stdout)
        self.assertIn("ok   libsvtav1 accepts a qpfile", result.stdout)
        self.assertIn("ok   libaom-av1 accepts a qpfile", result.stdout)

    def test_missing_fixture_is_a_fail(self) -> None:
        (self.yuv / "src01_hrc01_576x324.yuv").unlink()
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL required fixture missing", result.stdout)

    def test_unlaunchable_binary_is_a_fail(self) -> None:
        result = self.run_checker("/nonexistent/ffmpeg-3351")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL ffmpeg is not runnable", result.stdout)
        self.assertNotIn("Traceback", result.stdout + result.stderr)

    def test_missing_encoder_is_a_fail(self) -> None:
        result = self.run_checker(env={"STUB_ENCODERS": "libx264,libaom-av1"})
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL required encoder missing: libsvtav1", result.stdout)
