#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check the configure command the platform setup scripts print at the end.

Meson's source directory is `core/`; the repository root has no meson.build.
A `meson setup build` hint without the source directory fails with "Neither
source directory ... contains a build file meson.build". Every `meson setup`
line in scripts/setup/ must therefore name `core` as the source directory.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SETUP_DIR = REPO / "scripts" / "setup"
SETUP_LINE = re.compile(r"meson setup\s+(?P<args>[^\"'\n]*)")


MESON_SETUP_ARGS = 2  # positional arguments of `meson setup <builddir> <sourcedir>`


def configure_hints(text: str) -> list[str]:
    """Return the argument string of every `meson setup` mention in a script."""
    return [m.group("args").strip() for m in SETUP_LINE.finditer(text)]


def names_core_source_dir(args: str) -> bool:
    """True when the positional arguments are a build directory and `core`."""
    positional = [a for a in args.split() if not a.startswith("-")]
    return len(positional) >= MESON_SETUP_ARGS and positional[1] == "core"


class SetupScriptsConfigureHint(unittest.TestCase):
    def test_every_setup_script_names_the_core_source_dir(self) -> None:
        scripts = sorted(SETUP_DIR.glob("*.sh")) + sorted(SETUP_DIR.glob("*.ps1"))
        self.assertTrue(scripts, f"no setup scripts under {SETUP_DIR}")
        checked = 0
        for script in scripts:
            for args in configure_hints(script.read_text(encoding="utf-8")):
                checked += 1
                with self.subTest(script=script.name, args=args):
                    self.assertTrue(
                        names_core_source_dir(args),
                        f"{script.name}: `meson setup {args}` lacks the core source dir",
                    )
        self.assertGreater(checked, 0, "no `meson setup` line found in scripts/setup")

    def test_detector_refuses_a_hint_without_core(self) -> None:
        self.assertFalse(names_core_source_dir("build -Denable_cuda=false -Denable_sycl=true"))
        self.assertFalse(names_core_source_dir("build"))
        self.assertTrue(names_core_source_dir("build core -Denable_cuda=false"))


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
