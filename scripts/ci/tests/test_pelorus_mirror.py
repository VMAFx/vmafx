#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the shared Pelorus mirror lint/format manifest."""

from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
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
        """Assert the wrapper formats only the C++ file outside the mirror manifest."""
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
            ["/hook/bin/clang-format", "-i", "-style=file", escaped],
            check=False,
            timeout=mirror.CC_TIMEOUT_S,
        )

    def test_clang_format_wrapper_fails_when_formatter_is_missing(self) -> None:
        with (
            patch.object(mirror.shutil, "which", return_value=None),
            patch.object(mirror.subprocess, "run") as run,
        ):
            self.assertEqual(mirror.run_clang_format(["core/src/owned.c"]), 127)
        run.assert_not_called()


class LoadManifestCommand(unittest.TestCase):
    """The load-manifest command reuses load_manifest() for every exact-path list.

    The assertion-density exemption manifest (issue #3347) is a second exact-path
    list beside the mirror manifest; it must go through the same loader rather
    than grow a second matcher.
    """

    def _write(self, tmp: Path, content: str) -> Path:
        manifest = tmp / "manifest.txt"
        manifest.write_text(content, encoding="utf-8", newline="\n")
        return manifest

    def test_prints_entries_of_a_valid_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            manifest = self._write(
                Path(tmp),
                "# comment lines are ignored\n" "\n" "core/src/a.c\n" "core/src/b.c\n",
            )
            stdout = io.StringIO()
            with patch.object(sys, "stdout", stdout):
                self.assertEqual(mirror.main(["load-manifest", str(manifest)]), 0)
            self.assertEqual(stdout.getvalue().splitlines(), ["core/src/a.c", "core/src/b.c"])

    def test_refuses_an_unsorted_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            manifest = self._write(Path(tmp), "core/src/b.c\ncore/src/a.c\n")
            stderr = io.StringIO()
            with patch.object(sys, "stderr", stderr):
                self.assertEqual(mirror.main(["load-manifest", str(manifest)]), 1)
            self.assertIn("not sorted", stderr.getvalue())

    def test_refuses_duplicate_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            manifest = self._write(Path(tmp), "core/src/a.c\ncore/src/a.c\n")
            stderr = io.StringIO()
            with patch.object(sys, "stderr", stderr):
                self.assertEqual(mirror.main(["load-manifest", str(manifest)]), 1)
            self.assertIn("duplicates", stderr.getvalue())

    def test_refuses_a_path_outside_the_repository_form(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            manifest = self._write(Path(tmp), "core/src/../escape.c\n")
            stderr = io.StringIO()
            with patch.object(sys, "stderr", stderr):
                self.assertEqual(mirror.main(["load-manifest", str(manifest)]), 1)
            self.assertIn("invalid", stderr.getvalue())

    def test_missing_manifest_fails_with_an_error(self) -> None:
        stderr = io.StringIO()
        with patch.object(sys, "stderr", stderr):
            self.assertEqual(mirror.main(["load-manifest", "core/nonexistent/manifest.txt"]), 1)
        self.assertIn("error:", stderr.getvalue())

    def test_wrong_argument_count_is_a_usage_error(self) -> None:
        with self.assertRaises(SystemExit):
            mirror.main(["load-manifest"])


if __name__ == "__main__":
    unittest.main()
