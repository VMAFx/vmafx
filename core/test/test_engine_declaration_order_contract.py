#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""engine.h includes every public header that declares a function it redeclares.

`core/src/vmafx/engine_names_gen.h` renames each libvmaf function the engine
keeps to `vmaf_engine_<stem>`, so the public header's `VMAF_EXPORT` declaration
and `core/src/vmafx/engine.h`'s plain one name the same function. MSVC accepts
the plain declaration after the exported one and rejects the reverse with
C2375 ("redefinition; different linkage"). A translation unit that includes
`engine.h` before the public header therefore fails to compile on Windows only:
`vmafx_context_frames.c` did, with `libvmaf/perceptual_weight.h`, and kept the
Windows ARM64 MSVC, MSVC+CUDA and MSVC+CUDA (full) builds red on master.

The contract: `engine.h` includes the public header of every function it
redeclares, before its own declarations. Device-free and compiler-free: reads
the sources only.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CORE = Path(__file__).resolve().parents[1]
ENGINE = CORE / "src" / "vmafx" / "engine.h"
NAMES = CORE / "src" / "vmafx" / "engine_names_gen.h"
PUBLIC = CORE / "include" / "libvmaf"

RENAME = re.compile(r"^#define\s+(vmaf_\w+)\s+(vmaf_engine_\w+)\s*$", re.M)
EXPORTED = re.compile(r"VMAF_EXPORT\b[^;{]*?\b(vmaf_\w+)\s*\(", re.S)
INCLUDE = re.compile(r'^#include\s+"(libvmaf/[\w.]+)"', re.M)


def redeclared_public_names(engine_text: str, names_text: str) -> dict[str, str]:
    """Public libvmaf name -> engine name, for each engine name engine.h declares."""
    declared = set(re.findall(r"\b(vmaf_engine_\w+)\s*\(", engine_text))
    return {pub: eng for pub, eng in RENAME.findall(names_text) if eng in declared}


def declaring_header(public_name: str, headers: dict[str, str]) -> str | None:
    for header, text in headers.items():
        if public_name in EXPORTED.findall(text):
            return header
    return None


class EngineDeclarationOrderTest(unittest.TestCase):
    def setUp(self) -> None:
        self.engine = ENGINE.read_text(encoding="utf-8")
        self.names = NAMES.read_text(encoding="utf-8")
        self.headers = {
            f"libvmaf/{p.name}": p.read_text(encoding="utf-8") for p in sorted(PUBLIC.glob("*.h"))
        }

    def test_engine_header_includes_every_public_header_it_redeclares(self) -> None:
        included = set(INCLUDE.findall(self.engine))
        pairs = redeclared_public_names(self.engine, self.names)
        self.assertGreater(len(pairs), 10, "the rename table was not read")
        missing = {}
        for public in sorted(pairs):
            header = declaring_header(public, self.headers)
            if header is not None and header not in included:
                missing.setdefault(header, []).append(pairs[public])
        self.assertEqual(missing, {}, "engine.h redeclares these without including their header")

    def test_planted_missing_include_is_refused(self) -> None:
        stripped = self.engine.replace('#include "libvmaf/perceptual_weight.h"\n', "")
        included = set(INCLUDE.findall(stripped))
        pairs = redeclared_public_names(stripped, self.names)
        header = declaring_header("vmaf_set_perceptual_weight_enabled", self.headers)
        self.assertEqual(header, "libvmaf/perceptual_weight.h")
        self.assertIn("vmaf_set_perceptual_weight_enabled", pairs)
        self.assertNotIn(header, included)


if __name__ == "__main__":
    unittest.main()
