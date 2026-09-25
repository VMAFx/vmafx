#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/check-tidy-build-dir.py (ADR-1323)."""

from __future__ import annotations

import json
import re
import runpy
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ci/check-tidy-build-dir.py"
MOD = runpy.run_path(str(SCRIPT))
check_build_dir = MOD["check_build_dir"]
MAKEFILE = ROOT / "Makefile"


class CheckTidyBuildDirTests(unittest.TestCase):
    def test_gpu_lane_rejects_in_repo_build_dir(self) -> None:
        repo_root = Path("/fake/repo")
        in_repo = Path("/fake/repo/build")
        for lane in ("cuda", "hip", "sycl"):
            with self.subTest(lane=lane):
                errors = check_build_dir(lane, in_repo, repo_root)
                self.assertTrue(
                    any("requires an out-of-repo build directory" in e for e in errors),
                    f"expected out-of-repo rejection for lane {lane}, got: {errors}",
                )

    def test_gpu_lane_rejects_missing_intro_buildoptions(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_root = Path("/fake/repo")
            build_dir = Path(tmpdir)
            for lane in ("cuda", "hip", "sycl"):
                with self.subTest(lane=lane):
                    errors = check_build_dir(lane, build_dir, repo_root)
                    self.assertTrue(
                        any("intro-buildoptions.json' not found" in e for e in errors),
                        f"expected missing options error for lane {lane}, got: {errors}",
                    )

    def test_gpu_lane_rejects_missing_b_lto(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_root = Path("/fake/repo")
            build_dir = Path(tmpdir)
            meson_info = build_dir / "meson-info"
            meson_info.mkdir(parents=True)
            (meson_info / "intro-buildoptions.json").write_text(
                json.dumps([{"name": "some_other_opt", "value": True}]),
                encoding="utf-8",
            )
            for lane in ("cuda", "hip", "sycl"):
                with self.subTest(lane=lane):
                    errors = check_build_dir(lane, build_dir, repo_root)
                    self.assertTrue(
                        any("missing the b_lto option" in e for e in errors),
                        f"expected missing b_lto rejection for lane {lane}, got: {errors}",
                    )

    def test_gpu_lane_rejects_b_lto_true(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_root = Path("/fake/repo")
            build_dir = Path(tmpdir)
            meson_info = build_dir / "meson-info"
            meson_info.mkdir(parents=True)
            (meson_info / "intro-buildoptions.json").write_text(
                json.dumps([{"name": "b_lto", "value": True}]),
                encoding="utf-8",
            )
            for lane in ("cuda", "hip", "sycl"):
                with self.subTest(lane=lane):
                    errors = check_build_dir(lane, build_dir, repo_root)
                    self.assertTrue(
                        any("requires -Db_lto=false" in e for e in errors),
                        f"expected b_lto=false rejection for lane {lane}, got: {errors}",
                    )

    def test_gpu_lane_accepts_valid_out_of_repo_lto_false(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_root = Path("/fake/repo")
            build_dir = Path(tmpdir)
            meson_info = build_dir / "meson-info"
            meson_info.mkdir(parents=True)
            (meson_info / "intro-buildoptions.json").write_text(
                json.dumps([{"name": "b_lto", "value": False}]),
                encoding="utf-8",
            )
            for lane in ("cuda", "hip", "sycl"):
                with self.subTest(lane=lane):
                    errors = check_build_dir(lane, build_dir, repo_root)
                    self.assertEqual(errors, [])

    def test_cpu_lane_allows_in_repo_build_dir(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_root = Path(tmpdir)
            in_repo = repo_root / "build"
            meson_info = in_repo / "meson-info"
            meson_info.mkdir(parents=True)
            (meson_info / "intro-buildoptions.json").write_text(
                json.dumps([{"name": "b_lto", "value": False}]),
                encoding="utf-8",
            )
            errors = check_build_dir("cpu", in_repo, repo_root)
            self.assertEqual(errors, [])

    def test_makefile_invokes_check_tidy_build_dir(self) -> None:
        text = MAKEFILE.read_text(encoding="utf-8")
        for target in ("tidy-ratchet", "tidy-ratchet-write"):
            with self.subTest(target=target):
                match = re.search(
                    rf"^{re.escape(target)}:[^\n]*\n((?:\t[^\n]*\n)+)", text, re.MULTILINE
                )
                self.assertIsNotNone(match, f"target {target} missing from Makefile")
                lines = [ln.strip() for ln in match.group(1).splitlines()]
                self.assertTrue(
                    any("check-tidy-build-dir.py" in ln for ln in lines),
                    f"{target} recipe must invoke check-tidy-build-dir.py",
                )


if __name__ == "__main__":
    unittest.main()
