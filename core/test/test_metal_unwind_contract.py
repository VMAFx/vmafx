#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Require every Metal init unwind helper to guard the context and keep the init error.

The unwind helpers of core/src/feature/metal/*.mm release a failed init in
reverse order. The lifecycle and the context are released only inside an
``if (s->ctx != nullptr)`` block, so a failed vmaf_metal_context_new() never
reaches the release calls, and the helper returns the error it was given.
Device resource releases before the guard must precede context destruction,
no device releases may follow the guard, and every bridged transfer in an
unwind helper or per-file release/free helper must be nulled.
The check verifies ordering and nulling only; it does not claim detection
of an omitted release.
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
# A device resource release: a pipeline handed back to ARC, a kernel buffer
# freed, or a per-file release/free helper. Each one belongs to the context, so
# it must run before the guarded context release. Host-memory helpers
# (free_host_*) need no context and may run after it.
RESOURCE = re.compile(
    r"__bridge_transfer|vmaf_metal_kernel_buffer_free\(|\b\w*(?:release|free)_(?!host_)\w+\(s\)"
)
RELEASE_HELPER = re.compile(
    r"^(?:static\s+)?(?:\w+\s*\*?\s*)\b(\w*(?:release|free)_(?!host_)\w+)\s*\([^)]*\)\n\{\n(.*?)^\}\n",
    re.M | re.S,
)
BRIDGED = re.compile(
    r"__bridge_transfer [^)]*\)\s*\(?([*a-zA-Z0-9_>\[\]-]+)\)?;\s*([*a-zA-Z0-9_>\[\]-]+)\s*=\s*nullptr;"
)


def transfer_problems(name: str, body: str) -> list[str]:
    """Return a problem if any __bridge_transfer is not followed by nulling the field."""
    transfers = body.count("__bridge_transfer")
    nulled = [m for m in BRIDGED.finditer(body) if m.group(1) == m.group(2)]
    if len(nulled) != transfers:
        return [f"{name}: a transferred pipeline is not set to nullptr"]
    return []


def resource_problems(name: str, body: str) -> list[str]:
    """Return ordering violations around the s->ctx guard or dangling bridged transfers."""
    problems: list[str] = []
    guard = GUARD.search(body)
    before, after = (body[: guard.start()], body[guard.end() :]) if guard else (body, "")
    if not RESOURCE.search(before):
        problems.append(f"{name}: releases no resource before the s->ctx guard")
    if RESOURCE.search(after):
        problems.append(f"{name}: releases a resource after the context is destroyed")
    problems += transfer_problems(name, body)
    return problems


def release_problems(text: str, label: str) -> list[str]:
    """Return nulling problems for release/free helpers in the file text."""
    problems: list[str] = []
    for m in RELEASE_HELPER.finditer(text):
        fn_name, body = m.group(1), m.group(2)
        if "__bridge_transfer" in body:
            problems += transfer_problems(f"{label}:{fn_name}", body)
    return problems


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
    problems += resource_problems(name, body)
    if not body.rstrip().endswith("return err;"):
        problems.append(f"{name}: does not end with return err")
    return problems


def file_problems(text: str, label: str) -> tuple[int, list[str]]:
    """Return the unwind helper count and order/nulling problems in the file text."""
    found = list(HELPER.finditer(text))
    problems: list[str] = []
    for m in found:
        problems += helper_problems(f"{label}:{m.group(1)}", m.group(2))
    problems += release_problems(text, label)
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
            "    release_buffers(s);\n"
            "    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);\n"
            "    vmaf_metal_context_destroy(s->ctx);\n"
            "    return 0;\n}\n"
        )
        count, problems = file_problems(planted, "planted")
        self.assertEqual(count, 1)
        self.assertEqual(len(problems), 5, problems)

    def test_missing_pre_guard_release_is_refused(self) -> None:
        """Negative: a helper with a guard but no resource release before it is reported."""
        planted = (
            "static int unwind_x(State *s, int err)\n{\n"
            "    if (s->ctx != nullptr) {\n"
            "        (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);\n"
            "        vmaf_metal_context_destroy(s->ctx);\n"
            "    }\n"
            "    return err;\n}\n"
        )
        _, problems = file_problems(planted, "planted")
        self.assertEqual(
            problems,
            ["planted:unwind_x: releases no resource before the s->ctx guard"],
        )

    def test_late_or_dangling_resource_is_refused(self) -> None:
        """Negative: a device release after the guard and an un-nulled pipeline are reported."""
        planted = (
            "static int unwind_x(State *s, int err)\n{\n"
            "    if (s->pso) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso; }\n"
            "    if (s->ctx != nullptr) {\n"
            "        (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);\n"
            "        vmaf_metal_context_destroy(s->ctx);\n"
            "    }\n"
            "    release_buffers(s);\n"
            "    return err;\n}\n"
        )
        _, problems = file_problems(planted, "planted")
        self.assertEqual(
            problems,
            [
                "planted:unwind_x: releases a resource after the context is destroyed",
                "planted:unwind_x: a transferred pipeline is not set to nullptr",
            ],
        )

    def test_every_release_helper_nulls_transfers(self) -> None:
        """Positive: for each expected file, every release helper nulls transferred fields."""
        for name in sorted(EXPECTED_HELPERS):
            with self.subTest(name=name):
                problems = release_problems((METAL_ROOT / name).read_text(), name)
                self.assertEqual(problems, [])

    def test_release_helper_transfer_without_nulling_is_refused(self) -> None:
        """Negative: a release helper transferring without setting nullptr is reported."""
        planted = (
            "static void release_psos(State *s)\n{\n"
            "    if (s->pso) { (void)(__bridge_transfer id)s->pso; }\n"
            "}\n"
            "void release_object(void **handle)\n{\n"
            "    if (*handle != nullptr) {\n"
            "        (void)(__bridge_transfer id)*handle;\n"
            "    }\n"
            "}\n"
        )
        _, problems = file_problems(planted, "planted")
        self.assertEqual(
            problems,
            [
                "planted:release_psos: a transferred pipeline is not set to nullptr",
                "planted:release_object: a transferred pipeline is not set to nullptr",
            ],
        )

    def test_host_free_after_guard_is_allowed(self) -> None:
        """Boundary: freeing host memory after the context release is not a finding."""
        planted = (
            "static int unwind_x(State *s, int err)\n{\n"
            "    release_buffers(s);\n"
            "    if (s->ctx != nullptr) {\n"
            "        vmaf_metal_context_destroy(s->ctx);\n"
            "    }\n"
            "    free_host_buffers(s);\n"
            "    return err;\n}\n"
        )
        self.assertEqual(file_problems(planted, "planted"), (1, []))


if __name__ == "__main__":
    unittest.main()
