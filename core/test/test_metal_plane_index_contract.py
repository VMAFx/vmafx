#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every Metal twin that indexes five moment planes in uint refuses a plane past 2^32 / 5.

T-METAL-UINT-PLANE-INDEX-2026-10-05. The kernels of float_vif, integer SSIM,
float_ssim and float_ms_ssim keep five planes of N samples in one buffer and
index them as `k * N + at` in `uint`, k up to 4. Each host calls
vmaf_mtl_plane_index_check() (core/src/feature/metal/metal_plane_index.h)
with VMAF_MTL_MOMENT_PLANES, or its own five-plane constant, in the function
that sizes the frame, which init() runs before it creates a Metal context.

Device-free: reads the kernels and the hosts. The checks are that the
largest plane multiplier of each kernel is 4 (so five planes), that the host
function calls the check with five planes, and that init() calls that
function before vmaf_metal_context_new(). It also reports a host without the
call (planted below).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

METAL = Path(__file__).resolve().parents[1] / "src" / "feature" / "metal"
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# twin: (kernel, host, function that sizes the frame, five-plane constants it may pass)
TWINS = {
    "float_vif": ("float_vif.metal", "float_vif_metal.mm", "init_geometry"),
    "integer_ssim": ("integer_ssim.metal", "integer_ssim_metal.mm", "configure"),
    "float_ssim": ("float_ssim.metal", "float_ssim_metal.mm", "configure"),
    "float_ms_ssim": ("float_ms_ssim.metal", "float_ms_ssim_metal.mm", "validate_dimensions"),
}
FIVE_PLANES = {"VMAF_MTL_MOMENT_PLANES", "ISSIM_MOMENT_PLANES", "5u", "5"}
CHECK = re.compile(r"vmaf_mtl_plane_index_check\s*\((?P<args>[^;]*)\)\s*;", re.S)
MULTIPLIER = re.compile(r"\b(\d+)u?\s*\*\s*(?:geometry\.)?plane\b")


def clean(text: str) -> str:
    return COMMENT.sub(" ", text)


def function(text: str, name: str) -> str:
    """The body of function `name` in comment-free `text`, or an empty string."""
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", text)
    if match is None:
        return ""
    depth, index = 0, match.end() - 1
    for index in range(match.end() - 1, len(text)):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        if depth == 0:
            break
    return text[match.start() : index + 1]


def host_problems(twin: str, text: str) -> list[str]:
    """Why the host of `twin` (comment-free `text`) can index past the uint range."""
    _, _, sizer = TWINS[twin]
    body = function(text, sizer)
    if not body:
        return [f"{twin}: {sizer}() not found"]
    call = CHECK.search(body)
    if call is None:
        return [f"{twin}: {sizer}() does not call vmaf_mtl_plane_index_check()"]
    planes = call.group("args").rsplit(",", 1)[-1].strip()
    if planes not in FIVE_PLANES:
        return [f"{twin}: the check counts {planes} planes, not five"]
    init = function(text, "init_fex_metal")
    sized, context = init.find(f"{sizer}("), init.find("vmaf_metal_context_new(")
    if sized < 0 or (context >= 0 and context < sized):
        return [f"{twin}: init_fex_metal() creates a context before {sizer}()"]
    return []


class LiveSources(unittest.TestCase):
    def test_every_kernel_indexes_five_planes(self) -> None:
        for twin, (kernel, _, _) in TWINS.items():
            with self.subTest(twin=twin):
                found = MULTIPLIER.findall(clean((METAL / kernel).read_text(encoding="utf-8")))
                self.assertEqual(max(int(k) for k in found), 4)

    def test_every_host_checks_before_its_context(self) -> None:
        for twin, (_, host, _) in TWINS.items():
            with self.subTest(twin=twin):
                text = clean((METAL / host).read_text(encoding="utf-8"))
                self.assertEqual(host_problems(twin, text), [])


class PlantedRegressions(unittest.TestCase):
    def test_a_host_without_the_check_is_reported(self) -> None:
        text = clean((METAL / "integer_ssim_metal.mm").read_text(encoding="utf-8"))
        mutated = re.sub(r"vmaf_mtl_plane_index_check\s*\(", "unchecked_plane_index(", text)
        self.assertNotEqual(mutated, text)
        self.assertEqual(
            host_problems("integer_ssim", mutated),
            ["integer_ssim: configure() does not call vmaf_mtl_plane_index_check()"],
        )

    def test_a_check_with_the_wrong_plane_count_is_reported(self) -> None:
        text = clean((METAL / "float_vif_metal.mm").read_text(encoding="utf-8"))
        mutated = text.replace("VMAF_MTL_MOMENT_PLANES)", "2u)", 1)
        self.assertNotEqual(mutated, text)
        self.assertIn(
            "float_vif: the check counts 2u planes, not five", host_problems("float_vif", mutated)
        )


if __name__ == "__main__":
    unittest.main()
