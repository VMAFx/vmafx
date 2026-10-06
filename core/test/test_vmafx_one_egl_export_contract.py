#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""One EGL export and one sync-object implementation for every lane (HISS-19).

`core/src/vmafx/egl_export.c` is the library's EGL dma-buf export of GL
textures (ADR-2132) and `core/src/vmafx/sync_object.c` its sync_file, dma-buf
fence and GL sync code (ADR-2091). The HIP and SYCL lanes once carried their
own copies; when they met on the integration branch the copies were folded
onto these files. This check refuses a lane that loads EGL or polls a
sync_file itself again, and a second definition of the sync-object
functions. The planted defects are the SYCL lane's former GL import and the
HIP lane's former sync_file poll.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src"
LANE_GL = ("cuda/import_gl.c", "hip/import_gl.c", "sycl/import_gl.c")
EGL_LOADING = ("eglGetProcAddress", "eglExportDMABUFImageMESA", "eglCreateImage")
SYNC_FUNCTIONS = (
    "vmafx_sync_file_wait",
    "vmafx_gl_sync_wait",
    "vmafx_sync_file_acquire",
    "vmafx_gl_sync_acquire",
)


def egl_problems(name: str, text: str) -> list[str]:
    """A lane's GL import that loads or calls EGL itself instead of egl_export.c."""
    found = [f"{name}: names {sym}" for sym in EGL_LOADING if sym in text]
    if name.startswith(("hip/", "sycl/")) and "vmafx_egl_export_planes(" not in text:
        found.append(f"{name}: does not export through vmafx_egl_export_planes()")
    return found


def definitions(text: str, function: str) -> int:
    """Definitions (not declarations or calls) of `function` in `text`."""
    return len(re.findall(rf"^\w[\w \*]*\b{function}\([^;]*\)\s*\n\{{", text, re.MULTILINE))


def sync_problems(sources: dict[str, str]) -> list[str]:
    found = []
    for function in SYNC_FUNCTIONS:
        where = sorted(name for name, text in sources.items() if definitions(text, function))
        if where != ["vmafx/sync_object.c"]:
            found.append(f"{function} defined in {where or 'no file'}, not only sync_object.c")
    return found


def library_sources() -> dict[str, str]:
    return {
        str(path.relative_to(SRC)): path.read_text(encoding="utf-8")
        for path in sorted(SRC.rglob("*.c"))
    }


class OneEglExport(unittest.TestCase):
    def test_lanes_export_through_egl_export(self) -> None:
        problems = []
        for name in LANE_GL:
            path = SRC / name
            if path.is_file():
                problems += egl_problems(name, path.read_text(encoding="utf-8"))
        self.assertEqual(problems, [])

    def test_sync_objects_defined_once(self) -> None:
        self.assertEqual(sync_problems(library_sources()), [])

    def test_refuses_a_lane_with_its_own_egl_loader(self) -> None:
        planted = (
            'static void egl_load(void) { egl_symbol(lib, get, "eglExportDMABUFImageMESA"); }\n'
        )
        self.assertNotEqual(egl_problems("sycl/import_gl.c", planted), [])

    def test_refuses_a_second_sync_file_poll(self) -> None:
        sources = dict(library_sources())
        sources["vmafx/sync_file.c"] = (
            "int vmafx_sync_file_wait(int fd, uint64_t timeout_ns)\n{\n    return 0;\n}\n"
        )
        self.assertIn(
            "vmafx_sync_file_wait defined in ['vmafx/sync_file.c', 'vmafx/sync_object.c'], "
            "not only sync_object.c",
            sync_problems(sources),
        )


if __name__ == "__main__":
    sys.exit(unittest.main())
