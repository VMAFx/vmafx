#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for compat ABI parity verification, plant detection, and boundary checks (ADR-3339)."""

from __future__ import annotations

import copy
import json
import math
import sys
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import upstream_parity_allowlist as allowlist  # noqa: E402
from scripts.dev import upstream_parity as guard  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures" / "upstream_parity"
CIEDE = "scalar/nflx422p10/F.ciede.default"
PSNR = "scalar/nflx8/F.psnr.default"


def _abi_documents() -> tuple[dict[str, Any], dict[str, Any]]:
    upstream = json.loads((FIXTURES / "upstream.json").read_text(encoding="utf-8"))
    fork = json.loads((FIXTURES / "fork.json").read_text(encoding="utf-8"))
    upstream["environment"]["abi"] = True
    fork["environment"]["abi"] = True
    return upstream, fork


def _fragments() -> tuple[allowlist.Fragment, ...]:
    return allowlist.load_fragments(FIXTURES / "allowlist.d", FIXTURES / "adr")


class CompatAbiParityTests(unittest.TestCase):
    def test_clean_compare_of_identical_abi_documents_passes(self) -> None:
        upstream, _ = _abi_documents()
        status, lines = guard.judge(upstream, copy.deepcopy(upstream), (), {})
        text = "\n".join(lines)
        self.assertEqual(status, guard.EXIT_PASS, text)
        self.assertIn("[ABI drop-in]", text)
        self.assertIn("upstream parity: PASS", text)

    def test_planted_defect_fails_exit_1_naming_difference(self) -> None:
        upstream, fork = _abi_documents()
        # Simulate LD_PRELOAD plant shifting pooled score by nextafter
        orig = float(upstream["runs"][PSNR]["pooled"]["mean|psnr_y"])
        planted = str(math.nextafter(orig, math.inf))
        fork["runs"][PSNR]["pooled"]["mean|psnr_y"] = planted
        status, lines = guard.judge(upstream, fork, _fragments(), {})
        text = "\n".join(lines)
        self.assertEqual(status, guard.EXIT_DIFFERENT, text)
        self.assertIn("NOT COVERED (1)", text)
        self.assertIn("psnr_y#pool-mean", text)
        self.assertIn("upstream parity: FAIL", text)

    def test_boundary_difference_within_fragment_bound_passes(self) -> None:
        upstream, fork = _abi_documents()
        # CIEDE has bound 0.2 in fixtures; 0.1 is well within bound
        upstream["runs"][CIEDE]["values"]["0|ciede2000"] = "33.1"
        fork["runs"][CIEDE]["values"]["0|ciede2000"] = "33.0"
        status, lines = guard.judge(upstream, fork, _fragments(), {})
        text = "\n".join(lines)
        self.assertEqual(status, guard.EXIT_PASS, text)
        self.assertIn("upstream parity: PASS", text)

    def test_boundary_difference_exceeding_fragment_bound_fails(self) -> None:
        upstream, fork = _abi_documents()
        # Bound is 0.2; 0.25 exceeds it
        upstream["runs"][CIEDE]["values"]["0|ciede2000"] = "33.25"
        fork["runs"][CIEDE]["values"]["0|ciede2000"] = "33.0"
        status, lines = guard.judge(upstream, fork, _fragments(), {})
        text = "\n".join(lines)
        self.assertEqual(status, guard.EXIT_DIFFERENT, text)
        self.assertIn("ABOVE BOUND (1)", text)

    def test_environment_line_contains_abi_tag(self) -> None:
        env = guard.describe_environment(("gcc", "g++"), "vmaf-dev-mcp:local", abi=True)
        line = guard.environment_line(env)
        self.assertIn("environment [ABI drop-in]:", line)

    def test_parse_args_abi_and_plant_flags(self) -> None:
        args = guard.parse_args(["--abi"])
        self.assertTrue(args.abi)
        self.assertFalse(args.plant)

        args = guard.parse_args(["--plant"])
        self.assertTrue(args.plant)
        self.assertTrue(args.abi)


class UpstreamBaselineTests(unittest.TestCase):
    def test_x86_64_upstream_build_takes_the_image_baseline(self) -> None:
        with mock.patch("platform.machine", return_value="x86_64"):
            args = guard.upstream_baseline_args()
        self.assertEqual(args, ["-Dc_args=-march=x86-64", "-Dcpp_args=-march=x86-64"])

    def test_other_machines_get_no_march(self) -> None:
        with mock.patch("platform.machine", return_value="aarch64"):
            self.assertEqual(guard.upstream_baseline_args(), [])

    def test_v3_default_is_not_the_baseline(self) -> None:
        with mock.patch("platform.machine", return_value="x86_64"):
            args = guard.upstream_baseline_args()
        self.assertFalse(any("x86-64-v" in arg for arg in args))


if __name__ == "__main__":
    unittest.main()
