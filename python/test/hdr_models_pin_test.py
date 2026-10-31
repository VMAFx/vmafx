# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pins for HDR VMAF models on tagged SDR content and PQ material.

Pins captured from Netflix upstream vmaf CLI at 4611e47a7 (6 decimals).
Tagged input: SDR content labelled PQ (src01_hrc00 / src01_hrc01, 16 frames).
PQ input: material has no motion, so HFR equals non-HFR there (ref_dock / dis_dock, 1 frame).
Sol Levante input: real PQ with motion, 8 frames at 1080p, from the fixtures-pq-sollevante-v1
release. Those rows hold 17 digits and compare exactly, since CPU scores match upstream 4611e47a7
bit for bit on this clip.
"""

from __future__ import annotations

import glob
import json
import os
import platform
import subprocess
import sys
import tempfile
import unittest

from vmaf import ExternalProgram
from vmaf.config import VmafConfig

# Set only on the CI leg built with -Denable_zimg=false: there a pin row asserts the refusal
# that names zimg. A build with zimg fails this assertion, so the setting cannot hide a row.
EXPECT_NO_ZIMG = os.environ.get("VMAF_EXPECT_NO_ZIMG") == "1"

# Decimal places of the pooled-mean pin. libvmaf scores identical converted pictures
# bit-exact on x86-64, aarch64 and macOS (run 41178767446, every feature of every frame);
# the difference sits in the zimg conversion, whose float path rounds differently on
# aarch64 and in the macOS build, up to 3.5e-4 on the pooled mean. x86-64 holds 5 places.
# Unknown machines stay strict. The reason is documented in docs/models/v1.md.
_NON_X86_CONVERSION = platform.machine().lower() in ("arm64", "aarch64") or sys.platform == "darwin"
HDR_PIN_PLACES = 3 if _NON_X86_CONVERSION else 5


def _pair_args(pair: tuple[str, str, int, int, str, int]) -> list[str]:
    """Return the input flags for one (ref, dis, width, height, subsampling, frames) pair."""
    ref, dis, width, height, subsampling, frames = pair
    return [
        "-r",
        ref,
        "-d",
        dis,
        "-w",
        str(width),
        "-h",
        str(height),
        "-p",
        subsampling,
        "-b",
        "10",
        "--frame_cnt",
        str(frames),
    ]


def _input_args(kind: str) -> list[str]:
    """Return command-line input flags for the named material."""
    if kind == "sollevante_pq":
        bundle = ("yuv", "pq-sollevante-v1")
        ref = VmafConfig.test_resource_path(*bundle, "sollevante_ref_1920x1080_8f.yuv")
        dis = VmafConfig.test_resource_path(*bundle, "sollevante_dis_1920x1080_8f.yuv")
        return [*_pair_args((ref, dis, 1920, 1080, "420", 8)), "--precision", "max"]
    if kind == "pq":
        ref = VmafConfig.test_resource_path(
            "yuv",
            "ref_dock-na-HDR_res2160_rate5p0_maxRate100000_bufSize200000_fps59p940_576x324_1frame.yuv",
        )
        dis = VmafConfig.test_resource_path(
            "yuv",
            "dis_dock-na-HDR_res1440_rate15p0_maxRate10000_bufSize20000_fps59p940_576x324_1frame.yuv",
        )
        return _pair_args((ref, dis, 576, 324, "420", 1))
    ref = VmafConfig.test_resource_path("yuv", "src01_hrc00_576x324.yuv422p10le.yuv")
    dis = VmafConfig.test_resource_path("yuv", "src01_hrc01_576x324.yuv422p10le.yuv")
    return _pair_args((ref, dis, 576, 324, "422", 16))


def _run_cli(model: str, kind: str, out_path: str) -> subprocess.CompletedProcess[str]:
    """Execute vmaf CLI with HDR color metadata and requested model."""
    cmd = [
        ExternalProgram.vmafexec,
        *_input_args(kind),
        "--color_range_ref",
        "limited",
        "--color_primaries_ref",
        "bt2020",
        "--color_trc_ref",
        "smpte2084",
        "--color_matrix_ref",
        "bt2020nc",
        "--color_range_dist",
        "limited",
        "--color_primaries_dist",
        "bt2020",
        "--color_trc_dist",
        "smpte2084",
        "--color_matrix_dist",
        "bt2020nc",
        "--model",
        f"version={model}",
        "--json",
        "-o",
        out_path,
    ]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=60, check=False)


class HdrModelsPinTest(unittest.TestCase):
    """HDR model pin regressions asserting the pooled vmaf mean to HDR_PIN_PLACES places."""

    def test_every_model_has_both_pin_rows(self) -> None:
        """A deleted row or a new model without pins turns this test red."""
        paths = glob.glob(VmafConfig.model_path("hdr_vmaf_v1.1.8*", "hdr_vmaf_v1.1.8*.json"))
        versions = [os.path.splitext(os.path.basename(path))[0] for path in paths]
        self.assertEqual(len(versions), 8, versions)
        for version in versions:
            stem = version.replace("hdr_vmaf_v1.1.8", "test_hdr_v118")
            for kind in ("tagged", "pq", "sollevante_pq"):
                self.assertTrue(hasattr(self, f"{stem}_{kind}"), f"{version} has no {kind} pin")

    def _assert_pin(self, model: str, kind: str, expected_pin: float) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_path = os.path.join(tmp, "out.json")
            proc = _run_cli(model, kind, out_path)
            if EXPECT_NO_ZIMG:
                self.assertNotEqual(proc.returncode, 0, "scored without zimg")
                self.assertIn("built without zimg", (proc.stderr or "") + (proc.stdout or ""))
                return
            if proc.returncode != 0:
                err = (proc.stderr or "") + " " + (proc.stdout or "")
                if "zimg" in err.lower() or "without zimg" in err.lower():
                    self.fail(f"vmaf was built without zimg support: {err.strip()}")
                self.fail(f"vmaf failed with exit {proc.returncode}: {err.strip()}")
            if not os.path.isfile(out_path):
                self.fail(f"vmaf output json was not created: {proc.stderr}")
            with open(out_path, encoding="utf-8") as f:
                data = json.load(f)
            mean_score = data["pooled_metrics"]["vmaf"]["mean"]
            if kind == "sollevante_pq":
                self.assertEqual(mean_score, expected_pin)
            else:
                self.assertAlmostEqual(mean_score, expected_pin, places=HDR_PIN_PLACES)

    def test_hdr_v118_1d5h_2160_tagged(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_1d5h_2160", "tagged", 43.06662)

    def test_hdr_v118_1d5h_2160_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_1d5h_2160", "pq", 97.210772)

    def test_hdr_v118_3d0h_tagged(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_3d0h", "tagged", 55.056913)

    def test_hdr_v118_3d0h_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_3d0h", "pq", 96.48199)

    def test_hdr_v118_3d0h_2160_tagged(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_3d0h_2160", "tagged", 59.986048)

    def test_hdr_v118_3d0h_2160_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_3d0h_2160", "pq", 103.048065)

    def test_hdr_v118_5d0h_tagged(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_5d0h", "tagged", 60.196823)

    def test_hdr_v118_5d0h_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_5d0h", "pq", 96.720935)

    def test_hdr_v118_hfr_1d5h_2160_tagged(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_1d5h_2160", "tagged", 48.457131)

    def test_hdr_v118_hfr_1d5h_2160_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_1d5h_2160", "pq", 97.210772)

    def test_hdr_v118_hfr_3d0h_tagged(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_3d0h", "tagged", 60.56022)

    def test_hdr_v118_hfr_3d0h_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_3d0h", "pq", 96.48199)

    def test_hdr_v118_hfr_3d0h_2160_tagged(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_3d0h_2160", "tagged", 65.241174)

    def test_hdr_v118_hfr_3d0h_2160_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_3d0h_2160", "pq", 103.048065)

    def test_hdr_v118_hfr_5d0h_tagged(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_5d0h", "tagged", 65.574654)

    def test_hdr_v118_hfr_5d0h_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_5d0h", "pq", 96.720935)

    def test_hdr_v118_1d5h_2160_sollevante_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_1d5h_2160", "sollevante_pq", 43.527062125194355)

    def test_hdr_v118_3d0h_sollevante_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_3d0h", "sollevante_pq", 64.13634599393518)

    def test_hdr_v118_3d0h_2160_sollevante_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_3d0h_2160", "sollevante_pq", 63.73667901118218)

    def test_hdr_v118_5d0h_sollevante_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_5d0h", "sollevante_pq", 72.13416560402298)

    def test_hdr_v118_hfr_1d5h_2160_sollevante_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_1d5h_2160", "sollevante_pq", 46.4093155397766)

    def test_hdr_v118_hfr_3d0h_sollevante_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_3d0h", "sollevante_pq", 67.18719246562176)

    def test_hdr_v118_hfr_3d0h_2160_sollevante_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_3d0h_2160", "sollevante_pq", 66.55809929004137)

    def test_hdr_v118_hfr_5d0h_sollevante_pq(self) -> None:
        self._assert_pin("hdr_vmaf_v1.1.8_hfr_5d0h", "sollevante_pq", 75.08676439039081)


if __name__ == "__main__":
    unittest.main()
