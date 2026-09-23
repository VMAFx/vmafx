# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify the synthetic SYCL compile database preserves diagnostic flags."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parents[3]


def load_generator() -> ModuleType:
    """Import the hyphenated compile-database generator by path."""
    spec = importlib.util.spec_from_file_location(
        "gen_sycl_compile_commands", ROOT / "scripts/ci/gen-sycl-compile-commands.py"
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot import scripts/ci/gen-sycl-compile-commands.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ClangTidyCommandTests(unittest.TestCase):
    def test_translates_driver_flags_without_disabling_diagnostics(self) -> None:
        generator = load_generator()
        translated = generator.clang_tidy_command(
            "/opt/intel/oneapi/compiler/2026/bin/icpx -fsycl "
            "-fsycl-targets=spir64_gen,spir64 -fno-sycl-rdc "
            "-Xsycl-target-backend=spir64_gen '-device pvc' -fp-model=precise "
            "-pedantic -Wall -Wextra -Werror -c ../unit.cpp -o unit.o"
        )

        self.assertTrue(translated.startswith("clang++ "))
        self.assertNotIn("-fsycl", translated)
        self.assertNotIn("-fno-sycl-rdc", translated)
        self.assertNotIn("-Xsycl-target-backend", translated)
        self.assertNotIn("-device", translated)
        self.assertIn("-ffp-model=precise", translated)
        self.assertIn("-pedantic", translated)
        self.assertIn("-Wall", translated)
        self.assertIn("-Wextra", translated)
        self.assertIn("-Werror", translated)
        self.assertNotIn(" -o ", translated)

    def test_translates_legacy_unscoped_xs_commands(self) -> None:
        generator = load_generator()
        translated = generator.clang_tidy_command(
            "icpx -fsycl -fsycl-targets=spir64_gen -Xs '-device pvc' -c ../legacy.cpp -o legacy.o"
        )
        self.assertNotIn("-Xs", translated)
        self.assertNotIn("-device", translated)

    def test_translates_unscoped_target_backend_argument(self) -> None:
        generator = load_generator()
        translated = generator.clang_tidy_command(
            "icpx -fsycl -Xsycl-target-backend '-device dg2' "
            "-pedantic-errors -c ../unscoped.cpp -o unscoped.o"
        )
        self.assertNotIn("-Xsycl-target-backend", translated)
        self.assertNotIn("-device", translated)
        self.assertIn("-pedantic-errors", translated)


if __name__ == "__main__":
    unittest.main()
