#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Require every Metal init unwind helper to guard the context and keep the init error.

The unwind helpers of core/src/feature/metal/*.mm release a failed init in
reverse order. The lifecycle and the context are released only inside an
``if (s->ctx != nullptr)`` block, so a failed vmaf_metal_context_new() never
reaches the release calls, and the helper returns the error it was given.
The check is device-free and runs on every host.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

METAL_ROOT = Path(__file__).resolve().parents[1] / "src" / "feature" / "metal"

EXPECTED_HELPERS = {
    "float_adm_metal.mm",
    "float_moment_metal.mm",
    "float_ms_ssim_metal.mm",
    "float_psnr_metal.mm",
    "integer_ciede_metal.mm",
    "integer_psnr_hvs_metal.mm",
    "ssimulacra2_metal.mm",
}

HELPER = re.compile(r"^static int (unwind_\w+)\(\w+ \*s, int err\)\n\{\n(.*?)^\}\n", re.M | re.S)
GUARD = re.compile(r"^    if \(s->ctx != nullptr\) \{\n(.*?)^    \}\n", re.M | re.S)
RELEASES = ("vmaf_metal_kernel_lifecycle_close(", "vmaf_metal_context_destroy(")


def helper_problems(name: str, body: str) -> list[str]:
    """Return what the unwind helper body does wrong, empty when it holds."""
    problems: list[str] = []
    guarded = "".join(m.group(1) for m in GUARD.finditer(body))
    unguarded = GUARD.sub("", body)
    for call in RELEASES:
        if call in unguarded:
            problems.append(f"{name}: {call.rstrip('(')} outside the s->ctx guard")
        if call in body and call not in guarded:
            problems.append(f"{name}: {call.rstrip('(')} not released under the guard")
    if not body.rstrip().endswith("return err;"):
        problems.append(f"{name}: does not end with return err")
    return problems


def file_problems(text: str, label: str) -> tuple[int, list[str]]:
    """Return the helper count of the file text and every problem found."""
    found = list(HELPER.finditer(text))
    problems: list[str] = []
    for m in found:
        problems += helper_problems(f"{label}:{m.group(1)}", m.group(2))
    return len(found), problems


class MetalUnwindContract(unittest.TestCase):
    def test_every_helper_guards_the_context(self) -> None:
        """Positive: each expected file has a helper and every helper holds."""
        for name in sorted(EXPECTED_HELPERS):
            with self.subTest(name=name):
                count, problems = file_problems((METAL_ROOT / name).read_text(), name)
                self.assertGreaterEqual(count, 1, f"{name}: no unwind helper found")
                self.assertEqual(problems, [])

    def test_inventory_is_complete(self) -> None:
        """Boundary: a new .mm with an unwind helper must join the inventory."""
        with_helpers = {p.name for p in METAL_ROOT.glob("*.mm") if HELPER.search(p.read_text())}
        self.assertEqual(with_helpers, EXPECTED_HELPERS)

    def test_unguarded_release_is_refused(self) -> None:
        """Negative: a release outside the guard and a lost init error are reported."""
        planted = (
            "static int unwind_x(State *s, int err)\n{\n"
            "    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);\n"
            "    vmaf_metal_context_destroy(s->ctx);\n"
            "    return 0;\n}\n"
        )
        count, problems = file_problems(planted, "planted")
        self.assertEqual(count, 1)
        self.assertEqual(len(problems), 5, problems)


if __name__ == "__main__":
    unittest.main()
