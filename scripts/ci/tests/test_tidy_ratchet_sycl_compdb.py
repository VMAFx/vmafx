#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The sycl tidy lane augments its compilation database before measuring.

meson emits the SYCL feature translation units as ``CUSTOM_COMMAND`` rules
(``icpx -fsycl``), so ``write-compile-commands.py`` -- which exports only the
native ``c_COMPILER`` / ``cpp_COMPILER`` rules -- never sees them.  Without a
second pass through ``gen-sycl-compile-commands.py`` the lane measures zero
SYCL feature TUs and ``scripts/ci/tidy-baseline-sycl.json`` silently records
an empty backend, which is how that baseline came to hold no SYCL source at
all.

Both ratchet targets must therefore expand a per-lane compile-database hook
between the native export and the measurement, that hook must name the
generator for ``sycl``, and it must stay empty for the lanes whose database
meson writes natively.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MAKEFILE = ROOT / "Makefile"
GENERATOR = "gen-sycl-compile-commands.py"
HOOK = "$(TIDY_RATCHET_COMPDB_$(LANE))"
TARGETS = ("tidy-ratchet", "tidy-ratchet-write")


def makefile() -> str:
    return MAKEFILE.read_text(encoding="utf-8")


def variable(name: str, text: str) -> str:
    """Return the (line-continued) value assigned to *name*."""
    match = re.search(rf"^{re.escape(name)}\s*:?=(.*(?:\\\n.*)*)$", text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"{name} is not defined in the Makefile")
    return match.group(1).replace("\\\n", " ").strip()


def recipe(target: str, text: str) -> list[str]:
    """Return the recipe lines (tab-prefixed) of *target*."""
    match = re.search(rf"^{re.escape(target)}:[^\n]*\n((?:\t[^\n]*\n)+)", text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"{target} has no recipe in the Makefile")
    return [line.strip() for line in match.group(1).splitlines()]


class SyclLaneAugmentsCompileCommands(unittest.TestCase):
    def test_sycl_hook_runs_the_generator(self) -> None:
        self.assertIn(GENERATOR, variable("TIDY_RATCHET_COMPDB_sycl", makefile()))

    def test_native_lanes_have_an_empty_hook(self) -> None:
        text = makefile()
        for lane in ("cpu", "cuda", "hip"):
            with self.subTest(lane=lane):
                self.assertEqual(variable(f"TIDY_RATCHET_COMPDB_{lane}", text), "")

    def test_both_targets_expand_the_hook_in_order(self) -> None:
        """The hook appends to a database that exists and is not yet measured."""
        text = makefile()
        for target in TARGETS:
            with self.subTest(target=target):
                lines = recipe(target, text)
                self.assertIn(HOOK, lines)
                export = next(i for i, ln in enumerate(lines) if "write-compile-commands.py" in ln)
                measure = next(i for i, ln in enumerate(lines) if "tidy-ratchet.py" in ln)
                hook = lines.index(HOOK)
                self.assertLess(export, hook, "the augmentation must follow the native export")
                self.assertLess(hook, measure, "the augmentation must precede the measurement")


if __name__ == "__main__":
    unittest.main()
