#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The changed-files clang-tidy job leaves out what a C parse cannot read.

``Tidy Changed`` in ``lint-and-format.yml`` runs clang-tidy on the C and C++
files a push or pull request changed, against the CPU build's compile database.
A file that only another lane can parse must be named in ``exclude_untidyable``;
``core/src/metal/objc_handle.h`` is Objective-C++ only, so it stops at its
``#error`` and ``#include <bit>`` when read as C, and the job failed on master
after the Metal clean-up changed it. This test runs the workflow's own filter.
"""

from __future__ import annotations

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github" / "workflows" / "lint-and-format.yml"
OBJC_ONLY = "core/src/metal/objc_handle.h"
CXX_ONLY = ("core/src/feature/ff_math.h", "core/src/feature/ciede_ff_math.h")
KEPT = (
    "core/src/metal/objc_handle_other.h",
    "core/src/feature/ff_math_other.h",
    "core/src/feature/metal/integer_adm_metal_host.c",
    "core/src/picture.c",
)


def run_filter(paths: list[str]) -> list[str]:
    """Run ``exclude_untidyable`` from the workflow over *paths*."""
    text = WORKFLOW.read_text(encoding="utf-8")
    match = re.search(r"^( *)exclude_untidyable\(\) \{\n(.*?)^\1\}\n", text, re.M | re.S)
    if match is None:
        raise AssertionError("exclude_untidyable() is not in lint-and-format.yml")
    script = "exclude_untidyable() {\n" + match.group(2) + "}\nexclude_untidyable\n"
    done = subprocess.run(  # noqa: S603 -- fixed argv; the script is the workflow's own filter
        ["bash", "-c", script],  # noqa: S607 -- bash from PATH, as the runner's shell
        input="\n".join(paths) + "\n",
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    )
    return done.stdout.split()


class TidyChangedExclusionTests(unittest.TestCase):
    def test_objective_cpp_header_is_excluded(self) -> None:
        self.assertEqual(run_filter([OBJC_ONLY, *KEPT]), list(KEPT))

    def test_cxx_only_ff_math_headers_are_excluded(self) -> None:
        self.assertEqual(run_filter([*CXX_ONLY, *KEPT]), list(KEPT))

    def test_the_exclusion_is_one_exact_path(self) -> None:
        self.assertEqual(run_filter(list(KEPT)), list(KEPT))


if __name__ == "__main__":
    unittest.main()
