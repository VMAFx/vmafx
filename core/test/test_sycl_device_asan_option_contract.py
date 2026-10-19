#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""`-Dsycl_device_asan=true` puts the device sanitizer on every SYCL compile and the link.

The SYCL translation units are Meson custom targets, so `-Dcpp_args=...` never
reaches their command line: a build configured that way instruments no kernel
and still loads the sanitizer layer through the link, and a planted read past a
plane passes unreported (`T-SYCL-DEVICE-SANITIZER-UNPROVEN-2026-10-05`,
`docs/backends/sycl/device-sanitizer.md`). The option routes one list,
`sycl_asan_args`, into `sycl_toolchain_args` (every extractor and test compile),
into the per-translation-unit override of the AOT skip table, and into
`sycl_link_args`.

Device-free. The check reads `core/meson_options.txt` and `core/src/meson.build`;
the planted cases drop each routing line and the check must refuse the text.
"""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OPTIONS = ROOT / "core" / "meson_options.txt"
SRC_MESON = ROOT / "core" / "src" / "meson.build"

DEFINE = "sycl_asan_args = ['-Xarch_device', '-fsanitize=address', '-g', '-O2']"
ROUTES = (
    "sycl_toolchain_args += sycl_asan_args",
    "sycl_link_args += sycl_asan_args",
    "] + sycl_asan_args\n            else",
    "tu_toolchain_args = ['-fsycl'] + sycl_asan_args",
)


def problems(options: str, meson: str) -> list[str]:
    """Return what is wrong with the option and its routing, empty when sound."""
    found: list[str] = []
    if "option('sycl_device_asan'" not in options:
        found.append("core/meson_options.txt does not declare sycl_device_asan")
    elif (
        "value: false"
        not in options.split("option('sycl_device_asan'", 1)[1].split("option(", 1)[0]
    ):
        found.append("sycl_device_asan must default to false")
    if DEFINE not in meson:
        found.append("sycl_asan_args is not the instrumented -O2 list (-g alone drops to -O0)")
    found.extend(f"missing route: {route!r}" for route in ROUTES if route not in meson)
    return found


class SyclDeviceAsanOptionContract(unittest.TestCase):
    def setUp(self) -> None:
        self.options = OPTIONS.read_text(encoding="utf-8")
        self.meson = SRC_MESON.read_text(encoding="utf-8")

    def test_live_tree_is_sound(self) -> None:
        self.assertEqual(problems(self.options, self.meson), [])

    def test_every_route_is_needed(self) -> None:
        for route in ROUTES:
            with self.subTest(route=route):
                self.assertTrue(problems(self.options, self.meson.replace(route, "")))

    def test_undeclared_option_refused(self) -> None:
        self.assertTrue(problems(self.options.replace("sycl_device_asan", "x"), self.meson))

    def test_default_on_refused(self) -> None:
        head, tail = self.options.split("option('sycl_device_asan'", 1)
        planted = (
            head + "option('sycl_device_asan'" + tail.replace("value: false", "value: true", 1)
        )
        self.assertTrue(problems(planted, self.meson))

    def test_debug_info_without_optimisation_refused(self) -> None:
        self.assertTrue(problems(self.options, self.meson.replace(", '-O2']", "]")))


if __name__ == "__main__":
    unittest.main()
