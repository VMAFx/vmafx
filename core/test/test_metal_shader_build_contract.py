#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every Metal kernel is built for the macOS the tester bundle runs on (ADR-1496).

`core/src/metal/meson.build` compiles each `core/src/feature/metal/*.metal`
with `xcrun metal -c`. Without `-mmacosx-version-min` the offline compiler
stamps the `.air` with the build machine's SDK version, and a metallib stamped
newer than the host's macOS refuses to load, so every Metal twin would fail
init on a tester's Mac with an older macOS than the hosted runner's SDK. The
kernels take one argument list with the language revision and the deployment
target, and the target is the one the macOS tester bundle builds its C code
for (`MACOSX_DEPLOYMENT_TARGET` in `scripts/ci/build-macos-tester-bundle.sh`).

Device-free: reads the build files only.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
METAL_MESON = ROOT / "core" / "src" / "metal" / "meson.build"
KERNEL_DIR = ROOT / "core" / "src" / "feature" / "metal"
BUNDLE_SCRIPT = ROOT / "scripts" / "ci" / "build-macos-tester-bundle.sh"

# macOS major version -> Metal language revision of that release (Metal Shading
# Language Specification 4.1, section 1.6.10).
METAL_REVISION_FOR_MACOS = {"13": "metal3.0", "14": "metal3.1", "15": "metal3.2"}

TARGET_ARGS = re.compile(r"metal_shader_target_args = \[([^\]]*)\]")
KERNEL_LIST = re.compile(r"metal_kernel_names = \[(.*?)\]", re.S)
DEPLOYMENT = re.compile(r"^export MACOSX_DEPLOYMENT_TARGET=(\d+)\.(\d+)$", re.M)


def meson_code() -> str:
    """meson.build without comments."""
    return "\n".join(line.split("#", 1)[0] for line in METAL_MESON.read_text().splitlines())


def target_args(code: str) -> list[str]:
    match = TARGET_ARGS.search(code)
    assert match, "core/src/metal/meson.build: no metal_shader_target_args list"
    return re.findall(r"'([^']+)'", match.group(1))


def kernel_names(code: str) -> list[str]:
    match = KERNEL_LIST.search(code)
    assert match, "core/src/metal/meson.build: no metal_kernel_names list"
    return re.findall(r"'(\w+)'", match.group(1))


def target_failures(args: list[str], bundle_major: str) -> list[str]:
    """Why the target arguments do not match the bundle's macOS floor."""
    failures = []
    wanted_min = f"-mmacosx-version-min={bundle_major}.0"
    if wanted_min not in args:
        failures.append(f"{wanted_min} is missing (the bundle builds for macOS {bundle_major})")
    wanted_std = f"-std={METAL_REVISION_FOR_MACOS.get(bundle_major, '?')}"
    if wanted_std not in args:
        failures.append(f"{wanted_std} is missing")
    return failures


class MetalShaderBuildContract(unittest.TestCase):
    def test_every_kernel_file_is_built(self) -> None:
        on_disk = sorted(path.stem for path in KERNEL_DIR.glob("*.metal"))
        self.assertEqual(sorted(kernel_names(meson_code())), on_disk)

    def test_every_kernel_command_takes_the_target_arguments(self) -> None:
        code = meson_code()
        loop = code[code.index("foreach _k : metal_kernel_names") :]
        loop = loop[: loop.index("endforeach")]
        self.assertIn(
            "'metal'] + metal_shader_target_args", loop.replace("\n", " ").replace("  ", " ")
        )
        # No kernel is compiled outside the loop, with arguments of its own.
        self.assertEqual(code.count("'metal',"), 0)

    def test_target_is_the_bundle_floor(self) -> None:
        match = DEPLOYMENT.search(BUNDLE_SCRIPT.read_text())
        self.assertIsNotNone(match, "the bundle script sets no MACOSX_DEPLOYMENT_TARGET")
        self.assertEqual(target_failures(target_args(meson_code()), match.group(1)), [])

    def test_a_missing_or_wrong_target_is_detected(self) -> None:
        self.assertEqual(len(target_failures(["-std=metal3.1"], "14")), 1)
        self.assertEqual(
            len(target_failures(["-std=metal3.2", "-mmacosx-version-min=14.0"], "14")), 1
        )
        self.assertEqual(target_failures(["-std=metal3.1", "-mmacosx-version-min=14.0"], "14"), [])


if __name__ == "__main__":
    unittest.main()
