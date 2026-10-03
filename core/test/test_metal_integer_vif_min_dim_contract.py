#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin integer_vif_metal's 16-pixel minimum (T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29).

Every scale of integer VIF reflects its filter taps once, which stays inside
the plane only while floor(dim / 2^s) exceeds the tap half-width: 16 pixels
for the {17, 9, 5, 3} filters. integer_vif_metal declared no minimum (its
init() refused only a zero-sized scale 3, frames below 8 pixels), and its
compute kernels' tile loads reflected once, which sent the samples beyond a
small scale that no output reads outside the buffers (16x16 at scale 1 loads
index 19 of 8, reflected to -5).

The port takes the HIP guard (ADR-1381, on ADR-1324; ADR-1498):
vif_metal_min_dim() = 16 from the CPU's filter widths (integer_vif.h), an
ADR-1324 context check that sends smaller frames to the CPU `vif` under model
dispatch, and -EINVAL from init() for a direct request before any device work.
The kernels' border index folds over the period 2 * (sup - 1)
(metal_integer_vif_math.h, held against the CPU's reflection by
test_metal_integer_vif_math), so every load stays inside the plane.

Device-free: reads the sources only. test_metal_integer_vif_parity
(declares_min_dim, direct_init_rejects_below_min, model_boundary) measures it
on a device.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import metal_option_tables as tables

FEATURE = tables.FEATURE
HOST = "metal/integer_vif_metal.mm"
KERNEL = "metal/integer_vif.metal"
MATH = "metal/metal_integer_vif_math.h"
REFERENCE = "integer_vif.h"

MIN_DIM = (
    '#include "integer_vif.h"',
    "for (unsigned scale = 0u; scale < (unsigned)IVIF_SCALES; scale++) {",
    "const unsigned need = (((unsigned)vif_filter1d_width[scale] / 2u) + 1u) << scale;",
    "? (((unsigned)vif_filter1d_width[scale + 1u] / 2u) + 1u) << scale",
)
CONTEXT_CHECK = (
    "const unsigned min_dim = vif_metal_min_dim();",
    "return (w < min_dim || h < min_dim) ? -ENOTSUP : 0;",
    ".context_check = check_context_metal,",
    '.context_fallback_name = "vif",',
)
INIT_GUARD = "if (w < min_dim || h < min_dim) {"
KERNEL_FOLD = (
    '#include "metal_integer_vif_math.h"',
    "return vmaf_mtl_vif_mirror(idx, sup);",
)
# A reflection of the kernel's own: one bounce, out of range beyond 2*(sup-1).
SINGLE_BOUNCE = re.compile(r"2\s*\*\s*\(\s*sup\s*-\s*1\s*\)\s*-\s*idx")
MATH_FOLD = (
    '#include "metal_portable.h"',
    "if (sup <= 1) { return 0; }",
    "const vmaf_mtl_i32 period = 2 * (sup - 1);",
    "vmaf_mtl_i32 m = idx % period;",
    "if (m < 0) { m += period; }",
    "return (m < sup) ? m : period - m;",
)
REFERENCE_LINES = ("static const int vif_filter1d_width[4] = {17, 9, 5, 3};",)


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(tables.strip_comments(source).split())


def _sources() -> dict[str, str]:
    names = (HOST, KERNEL, MATH, REFERENCE)
    return {name: (FEATURE / name).read_text(encoding="utf-8") for name in names}


def _missing(code: str, pieces: tuple[str, ...]) -> list[str]:
    return [piece for piece in pieces if " ".join(piece.split()) not in code]


def _init_body(code: str) -> str:
    start = code.find("static int init_fex_metal(")
    end = code.find("static int ", start + 1)
    return code[start:end] if start >= 0 else ""


def _host_failures(host: str) -> list[str]:
    code = _flat(host)
    failures: list[str] = []
    if _missing(code, MIN_DIM):
        failures.append(f"{HOST}: the minimum is not derived from the CPU's filter widths")
    if _missing(code, CONTEXT_CHECK):
        failures.append(f"{HOST}: no ADR-1324 context check with the CPU `vif` fallback")
    init = _init_body(code)
    guard = init.find(INIT_GUARD)
    refusal = init.find("return -EINVAL;", max(guard, 0))
    device = init.find("vmaf_metal_context_new(")
    if guard < 0 or refusal < 0 or device < 0 or not guard < refusal < device:
        failures.append(f"{HOST}: init() does not refuse a small frame before any device work")
    return failures


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    kernel = _flat(sources[KERNEL])
    failures: list[str] = []
    if _missing(kernel, KERNEL_FOLD) or SINGLE_BOUNCE.search(kernel):
        failures.append(f"{KERNEL}: the border index is not the shared fold")
    if _missing(_flat(sources[MATH]), MATH_FOLD):
        failures.append(f"{MATH}: the border index is not the fold over 2 * (sup - 1)")
    return failures


def _reference_failures(reference: str) -> list[str]:
    code = _flat(reference)
    return [f"{REFERENCE} no longer holds `{p}`" for p in _missing(code, REFERENCE_LINES)]


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _host_failures(sources[HOST])
        + _kernel_failures(sources)
        + _reference_failures(sources[REFERENCE])
    )


class MetalIntegerVifMinDimContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_missing_context_check_is_detected(self) -> None:
        failures = self._edited(HOST, "    .context_check         = check_context_metal,\n", "")
        self._assert_detected(failures, "no ADR-1324 context check")

    def test_wrong_fallback_is_detected(self) -> None:
        failures = self._edited(
            HOST, '.context_fallback_name = "vif",', '.context_fallback_name = "float_vif",'
        )
        self._assert_detected(failures, "no ADR-1324 context check")

    def test_missing_init_refusal_is_detected(self) -> None:
        # The pre-port init(): no guard, only a zero-sized scale 3 refused.
        failures = self._edited(
            HOST,
            "    if (w < min_dim || h < min_dim) {\n        vmaf_log(",
            "    if (false) {\n        vmaf_log(",
        )
        self._assert_detected(failures, "before any device work")

    def test_minimum_without_decimation_filters_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "? (((unsigned)vif_filter1d_width[scale + 1u] / 2u) + 1u) << scale",
            "? (((unsigned)vif_filter1d_width[scale] / 2u) + 1u) << scale",
        )
        self._assert_detected(failures, "not derived from the CPU's filter widths")

    def test_single_bounce_kernel_mirror_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    return vmaf_mtl_vif_mirror(idx, sup);",
            "    return (idx < 0) ? -idx : ((idx >= sup) ? 2 * (sup - 1) - idx : idx);",
        )
        self._assert_detected(failures, "not the shared fold")

    def test_fold_drift_is_detected(self) -> None:
        failures = self._edited(MATH, "        m += period;", "        m = -m;")
        self._assert_detected(failures, "fold over 2 * (sup - 1)")

    def test_cpu_filter_width_drift_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "static const int vif_filter1d_width[4] = {17, 9, 5, 3};",
            "static const int vif_filter1d_width[4] = {17, 9, 5, 5};",
        )
        self._assert_detected(failures, f"{REFERENCE} no longer holds")


if __name__ == "__main__":
    unittest.main()
