#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the shared Pelorus mirror lint/format manifest."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from types import ModuleType
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "pelorus_mirror.py"


def _load() -> ModuleType:
    spec = importlib.util.spec_from_file_location("pelorus_mirror", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


mirror = _load()


class ExactMirrorManifest(unittest.TestCase):
    def test_filter_exempts_only_manifest_members(self) -> None:
        exact = "core/src/interop/pelorus_interop.c"
        candidates = [
            exact,
            "core/src/interop/pelorus_escape.c",
            "core/src/interop/pelorus_escape.cpp",
            "core/src/interop/pelorus_escape.hpp",
            "core/include/libvmaf/pelorus/escape.h",
            "core/src/owned.c",
        ]
        self.assertEqual(mirror.filter_paths(candidates), candidates[1:])

    def test_native_suffix_contract_covers_c_cpp_and_headers(self) -> None:
        self.assertTrue(
            {
                ".c",
                ".cc",
                ".cpp",
                ".cxx",
                ".h",
                ".hh",
                ".hpp",
                ".hxx",
                ".cu",
                ".cuh",
                ".hip",
                ".inl",
                ".m",
                ".mm",
            }.issubset(mirror.NATIVE_SUFFIXES)
        )

    def test_clang_format_wrapper_keeps_unmanifested_cpp(self) -> None:
        exact = "core/src/interop/pelorus_interop.c"
        escaped = "core/src/interop/pelorus_escape.cpp"
        with (
            patch.object(mirror.shutil, "which", return_value="/hook/bin/clang-format"),
            patch.object(mirror.subprocess, "run") as run,
        ):
            run.return_value.returncode = 0
            self.assertEqual(
                mirror.run_clang_format(["-style=file", exact, escaped]),
                0,
            )
        run.assert_called_once_with(
            ["/hook/bin/clang-format", "-i", "-style=file", escaped], check=False
        )

    def test_clang_format_wrapper_fails_when_formatter_is_missing(self) -> None:
        with (
            patch.object(mirror.shutil, "which", return_value=None),
            patch.object(mirror.subprocess, "run") as run,
        ):
            self.assertEqual(mirror.run_clang_format(["core/src/owned.c"]), 127)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
