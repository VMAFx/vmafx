# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Positive, negative, and boundary controls for the hardware corpus CLI."""

from __future__ import annotations

import importlib.util
import json
import os
import unittest
from collections.abc import Callable
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "hw_encoder_corpus", ROOT / "scripts/dev/hw_encoder_corpus.py"
)
assert SPEC is not None and SPEC.loader is not None
HW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HW)


def _payload(*, with_metrics: bool = True) -> dict[str, Any]:
    metrics = dict.fromkeys(HW.CANONICAL_6, 0.5) if with_metrics else {}
    metrics["vmaf"] = 90.0
    return {
        "frames": [{"frameNum": 0, "metrics": metrics}],
        "pooled_metrics": {"vmaf": {"mean": 90.0}},
    }


class HardwareEncoderCorpusTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "source.yuv"
        self.vmaf = self.root / "vmaf"
        self.output = self.root / "out" / "rows.jsonl"
        self.source.write_bytes(b"source")
        # main() resolves --vmaf-bin and ffmpeg to executables before any work
        # (scripts/AGENTS.md); both are stubs here, ffmpeg found through PATH.
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        for tool in (self.vmaf, bin_dir / "ffmpeg"):
            tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            tool.chmod(0o755)
        path = mock.patch.dict(os.environ, {"PATH": f"{bin_dir}{os.pathsep}/usr/bin:/bin"})
        path.start()
        self.addCleanup(path.stop)
        self.argv = [
            "--vmaf-bin",
            str(self.vmaf),
            "--source",
            str(self.source),
            "--width",
            "16",
            "--height",
            "16",
            "--encoder",
            "libx264",
            "--cq",
            "23",
            "--out",
            str(self.output),
        ]
        self.addCleanup(self.temp.cleanup)

    @staticmethod
    def _encode_ok(*args: Any, **kwargs: Any) -> tuple[int, float, int]:
        args[7].write_bytes(b"mp4")
        return 0, 1.5, 4

    @staticmethod
    def _decode_ok(_mp4: Path, raw_yuv: Path, _pix_fmt: str, **_kwargs: Any) -> int:
        raw_yuv.write_bytes(b"yuv")
        return 0

    @staticmethod
    def _score_with(payload: dict[str, Any]) -> Callable[..., int]:
        def score(*args: Any) -> int:
            args[-1].write_text(json.dumps(payload), encoding="utf-8")
            return 0

        return score

    def test_success_writes_rows_and_returns_zero(self) -> None:
        with (
            mock.patch.object(HW, "encode_hw", side_effect=self._encode_ok),
            mock.patch.object(HW, "decode_to_raw", side_effect=self._decode_ok),
            mock.patch.object(HW, "score_cuda", side_effect=self._score_with(_payload())),
        ):
            self.assertEqual(HW.main(self.argv), 0)
        rows = [json.loads(line) for line in self.output.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["cq"], 23)

    def test_qsv_upload_follows_output_depth(self) -> None:
        for pix_fmt, extra, surface in (
            ("yuv420p", [], "nv12"),
            ("yuv420p10le", [], "p010le"),
            ("yuv420p", ["-pix_fmt", "yuv420p10le"], "p010le"),
            ("yuv420p", ["-profile:v", "main10"], "p010le"),
        ):
            with self.subTest(pix_fmt=pix_fmt, extra=extra):
                completed = mock.Mock(returncode=0, stdout="", stderr="")
                with mock.patch.object(HW, "run_command", return_value=completed) as run:
                    rc, _, _ = HW.encode_hw(
                        self.source,
                        320,
                        240,
                        pix_fmt,
                        24.0,
                        "hevc_qsv",
                        23,
                        self.root / "out.mp4",
                        extra=["-vf", "scale=320:240", *extra],
                    )
                self.assertEqual(rc, 0)
                argv = run.call_args.args[0]
                self.assertEqual(argv.count("-vf"), 1)
                self.assertEqual(
                    argv[argv.index("-vf") + 1],
                    f"scale=320:240,format={surface},hwupload=extra_hw_frames=64",
                )

    def test_encode_failure_returns_nonzero(self) -> None:
        with mock.patch.object(HW, "encode_hw", return_value=(7, 1.5, 0)):
            self.assertEqual(HW.main(self.argv), 1)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "")

    def test_empty_metric_payload_returns_nonzero(self) -> None:
        with (
            mock.patch.object(HW, "encode_hw", side_effect=self._encode_ok),
            mock.patch.object(HW, "decode_to_raw", side_effect=self._decode_ok),
            mock.patch.object(
                HW,
                "score_cuda",
                side_effect=self._score_with(_payload(with_metrics=False)),
            ),
        ):
            self.assertEqual(HW.main(self.argv), 1)

    def test_missing_source_returns_usage_error(self) -> None:
        self.source.unlink()
        self.assertEqual(HW.main(self.argv), 2)
        self.assertFalse(self.output.exists())

    def test_non_executable_vmaf_returns_usage_error(self) -> None:
        self.vmaf.chmod(0o644)
        self.assertEqual(HW.main(self.argv), 2)
        self.assertFalse(self.output.exists())

    def test_one_failed_point_keeps_the_others_rows_and_returns_nonzero(self) -> None:
        argv = [*self.argv, "--cq", "31"]
        good_then_bad = iter([(0, 1.5, 4), (7, 1.5, 0)])

        def encode(*args: Any, **_kwargs: Any) -> tuple[int, float, int]:
            rc, ms, size = next(good_then_bad)
            if rc == 0:
                args[7].write_bytes(b"mp4")
            return rc, ms, size

        with (
            mock.patch.object(HW, "encode_hw", side_effect=encode),
            mock.patch.object(HW, "decode_to_raw", side_effect=self._decode_ok),
            mock.patch.object(HW, "score_cuda", side_effect=self._score_with(_payload())),
        ):
            self.assertEqual(HW.main(argv), 1)
        rows = [json.loads(line) for line in self.output.read_text(encoding="utf-8").splitlines()]
        self.assertEqual([row["cq"] for row in rows], [23])


if __name__ == "__main__":
    unittest.main()
