#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the CPU behaviour of float_adm_metal (ADR-1498).

The Metal twin of ``float_adm`` takes the designs of the CUDA (ADR-1420),
SYCL (ADR-1434) and HIP (ADR-1458) twins. This contract holds the source to
them:

* ``init()`` refuses frames below 17x17 with the CPU's
  ``adm_frame_size_check()``, before it reads its state or claims a device
  resource (T-GPU-FLOAT-ADM-TINY-FRAME-FLOOR-2026-10-01).

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

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SPACE = re.compile(r"\s+")

SIZE_CHECK = 'adm_frame_size_check("float_adm_metal", w, h)'


def _code(source: str) -> str:
    """The source without comments and with whitespace collapsed."""
    return SPACE.sub(" ", COMMENT.sub(" ", source))


def _sources() -> dict[str, str]:
    return {name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in (HOST,)}


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


def _failures(sources: dict[str, str]) -> list[str]:
    host = _code(sources[HOST])
    return _size_check_failures(host)


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


if __name__ == "__main__":
    unittest.main()
