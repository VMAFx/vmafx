#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the CPU behaviour of float_adm_metal (ADR-1498).

The Metal twin of ``float_adm`` takes the designs of the CUDA (ADR-1420),
SYCL (ADR-1434) and HIP (ADR-1458) twins. This contract holds the source to
them:

* ``init()`` refuses frames below 17x17 with the CPU's
  ``adm_frame_size_check()``, before it reads its state or claims a device
  resource (T-GPU-FLOAT-ADM-TINY-FRAME-FLOOR-2026-10-01);
* the frame numerator and denominator are floored at ``compute_adm()``'s
  ``1e-10 * (w * h) / (1920.0 * 1080.0)``, not at the ``1e-2`` of a branch no
  build defines (T-GPU-FLOAT-ADM-FRAME-SUM-FLOOR-2026-10-01).

No Apple device runs anything here: the contract reads the sources, and each
check has a planted regression below. ``test_metal_float_adm_parity`` measures
the twin on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

HOST = "metal/float_adm_metal.mm"
CPU_FRAME = "adm.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SPACE = re.compile(r"\s+")

SIZE_CHECK = 'adm_frame_size_check("float_adm_metal", w, h)'
FRAME_FLOOR = "1e-10 * (w * h) / (1920.0 * 1080.0)"


def _code(source: str) -> str:
    """The source without comments and with whitespace collapsed."""
    return SPACE.sub(" ", COMMENT.sub(" ", source))


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in (HOST, CPU_FRAME)
    }


def _function_body(source: str, name: str) -> str:
    """Text of the first definition of `name` (brace-matched), or empty."""
    match = re.search(rf"\b{name}\([^;{{]*\)\s*\{{", source)
    if not match:
        return ""
    depth = 0
    for index in range(match.end() - 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : index + 1]
    return ""


def _size_check_failures(host: str) -> list[str]:
    """init() calls the CPU's size check first."""
    init = _function_body(host, "init_fex_metal")
    check = init.find(SIZE_CHECK)
    if check < 0:
        return [f"{HOST}: init_fex_metal() must call `{SIZE_CHECK}`"]
    failures: list[str] = []
    for later in ("fex->priv", "vmaf_metal_context_new(", "newBufferWithLength"):
        found = init.find(later)
        if 0 <= found < check:
            failures.append(f"{HOST}: init_fex_metal() must call the size check before `{later}`")
    return failures


def _floor_failures(sources: dict[str, str], host: str) -> list[str]:
    """The frame sums are floored where and as compute_adm() floors them."""
    failures: list[str] = []
    if FRAME_FLOOR not in _code(sources[CPU_FRAME]):
        failures.append(f"{CPU_FRAME}: compute_adm() no longer floors at `{FRAME_FLOOR}`")
    collect = _function_body(host, "collect_fex_metal")
    if f"const double numden_limit = {FRAME_FLOOR};" not in collect:
        failures.append(f"{HOST}: the frame floor must be compute_adm()'s `{FRAME_FLOOR}`")
    if re.search(r"\b1e-2\b", host):
        failures.append(f"{HOST}: the 1e-2 floor of ADM_OPT_SINGLE_PRECISION is back")
    return failures


def _failures(sources: dict[str, str]) -> list[str]:
    host = _code(sources[HOST])
    return _size_check_failures(host) + _floor_failures(sources, host)


class MetalFloatAdmExactContractTest(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _failures(sources)

    def _detects(self, failures: list[str], text: str) -> None:
        self.assertTrue(any(text in failure for failure in failures), failures)

    def test_live_sources_keep_the_cpu_behaviour(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_missing_size_check_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            f"    const int size_err = {SIZE_CHECK};",
            "    const int size_err = 0;",
        )
        self._detects(failures, "must call `adm_frame_size_check")

    def test_size_check_after_the_device_is_detected(self) -> None:
        sources = _sources()
        check = f"    const int size_err = {SIZE_CHECK};\n    if (size_err != 0) {{ return size_err; }}\n"
        context = "    int err = vmaf_metal_context_new(&s->ctx, 0);\n"
        self.assertIn(check, sources[HOST])
        self.assertIn(context, sources[HOST])
        sources[HOST] = sources[HOST].replace(check, "", 1).replace(context, context + check, 1)
        self._detects(_failures(sources), "before `vmaf_metal_context_new(`")

    def test_old_floor_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            f"    const double numden_limit = {FRAME_FLOOR};",
            "    const double numden_limit = 1e-2 * (double)(w * h) / (1920.0 * 1080.0);",
        )
        self._detects(failures, "the frame floor must be")
        self._detects(failures, "1e-2 floor")

    def test_changed_reference_floor_is_detected(self) -> None:
        failures = self._edited(
            CPU_FRAME,
            f"double numden_limit = {FRAME_FLOOR};",
            "double numden_limit = 1e-12 * (w * h) / (1920.0 * 1080.0);",
        )
        self._detects(failures, CPU_FRAME)


if __name__ == "__main__":
    unittest.main()
