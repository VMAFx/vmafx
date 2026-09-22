#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The ratchet forwards --extra-arg values to clang-tidy wrapped, never bare."""

from __future__ import annotations

import importlib.util
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parents[3]


def load_ratchet() -> ModuleType:
    """Import the hyphenated ratchet script under test by path."""
    spec = importlib.util.spec_from_file_location(
        "tidy_ratchet", ROOT / "scripts/ci/tidy-ratchet.py"
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot build an import spec for scripts/ci/tidy-ratchet.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["tidy_ratchet"] = module
    spec.loader.exec_module(module)
    return module


class ExtraArgForwarding(unittest.TestCase):
    def test_values_reach_clang_tidy_as_extra_arg(self) -> None:
        """A stub clang-tidy echoes its argv; the lane flags must arrive wrapped.

        This is the shape the Makefile's cuda and hip lanes use
        (``--extra-arg=--cuda-host-only``): argparse strips the wrapper, and
        before the fix the bare ``--cuda-host-only`` reached clang-tidy as an
        unknown option, so every translation unit counted as a compile failure.
        """
        ratchet = load_ratchet()
        with tempfile.TemporaryDirectory() as tmp:
            stub = Path(tmp) / "clang-tidy-stub"
            stub.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n", encoding="utf-8")
            stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
            source = Path(tmp) / "unit.c"
            source.write_text("int x;\n", encoding="utf-8")
            _src, output, rc = ratchet.run_one(
                str(stub),
                Path(tmp),
                ["--cuda-host-only", "-nocudalib", "--extra-arg=-x"],
                (source, Path(tmp)),
            )
        self.assertEqual(rc, 0)
        argv = output.split()
        self.assertIn("--extra-arg=--cuda-host-only", argv)
        self.assertIn("--extra-arg=-nocudalib", argv)
        self.assertIn("--extra-arg=-x", argv, "an already-wrapped value must pass through once")
        self.assertNotIn(
            "--cuda-host-only", argv, "the bare flag would be an unknown clang-tidy option"
        )
        self.assertNotIn("--extra-arg=--extra-arg=-x", argv, "no double wrapping")

    def test_relative_wrapper_path_survives_the_build_dir_cwd(self) -> None:
        """The sycl lane passes ``--clang-tidy scripts/ci/clang-tidy-sycl.sh``.

        run_one executes with ``cwd`` set to the translation unit's build
        directory, so a repository-relative wrapper path resolved there does
        not exist: ``[Errno 2] No such file or directory``. The path has to be
        resolved against the invocation cwd before the switch.
        """
        ratchet = load_ratchet()
        with tempfile.TemporaryDirectory() as tmp:
            here = Path(tmp) / "repo"
            build = Path(tmp) / "build"
            (here / "scripts").mkdir(parents=True)
            build.mkdir()
            stub = here / "scripts" / "wrapper.sh"
            stub.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n", encoding="utf-8")
            stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
            source = here / "unit.c"
            source.write_text("int x;\n", encoding="utf-8")
            previous = Path.cwd()
            os.chdir(here)
            try:
                _src, output, rc = ratchet.run_one("scripts/wrapper.sh", build, [], (source, build))
            finally:
                os.chdir(previous)
        self.assertEqual(rc, 0, output)
        self.assertIn(str(source), output.split())


class Arm64LaneFlags(unittest.TestCase):
    """The arm64 lane's cross flags are load-bearing (ADR-1283).

    Its compile database is produced by ``aarch64-linux-gnu-gcc`` from
    ``build-aux/aarch64-linux-gnu.ini``. clang-tidy parses those commands with
    its own driver, so without ``--target`` it reads ``<arm_neon.h>`` against
    the host's x86 headers and every NEON translation unit becomes a
    ``clang-diagnostic-error`` — ratchet exit 4, a failed measurement rather
    than a clean one. Without ``--sysroot`` libc resolves against the host.
    Dropping either flag silently turns the lane into the very defect it
    exists to close, so the Makefile's definition is pinned here.
    """

    def lane_flags(self) -> str:
        """Return TIDY_RATCHET_EXTRA_arm64's value, backslash continuations joined."""
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        joined = text.replace("\\\n", " ")
        for line in joined.splitlines():
            if line.startswith("TIDY_RATCHET_EXTRA_arm64"):
                return line.split(":=", 1)[1]
        self.fail("Makefile defines no TIDY_RATCHET_EXTRA_arm64 lane")
        raise AssertionError  # unreachable; keeps the return type honest

    def test_lane_forwards_target_and_sysroot(self) -> None:
        flags = self.lane_flags().split()
        self.assertIn("--extra-arg=--target=$(AARCH64_TARGET)", flags)
        self.assertIn("--extra-arg=--sysroot=$(AARCH64_SYSROOT)", flags)

    def test_lane_defaults_are_the_cross_packages_own_paths(self) -> None:
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("AARCH64_TARGET ?= aarch64-linux-gnu", text)
        self.assertIn("AARCH64_SYSROOT ?= /usr/aarch64-linux-gnu", text)

    def test_a_baseline_exists_for_the_lane(self) -> None:
        """A lane with no committed baseline measures nothing on the next run."""
        self.assertTrue((ROOT / "scripts/ci/tidy-baseline-arm64.json").is_file())


if __name__ == "__main__":
    unittest.main()
