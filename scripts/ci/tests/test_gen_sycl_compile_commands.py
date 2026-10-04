# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify the synthetic SYCL compile database preserves diagnostic flags."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
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
            "-fsycl-targets=spir64_gen,spir64 -fno-sycl-rdc --offload-compress "
            "--offload-compression-level=22 "
            "-Xsycl-target-backend=spir64_gen '-device pvc' -fp-model=precise "
            "-ffp-contract=off -foffload-fp32-prec-div -foffload-fp32-prec-sqrt "
            "-pedantic -Wall -Wextra -Werror -c ../unit.cpp -o unit.o"
        )

        self.assertTrue(translated.startswith("clang++ "))
        # ADR-1367: the device precision pair is icpx-only; contraction-off is
        # a host-visible FP flag stock clang accepts, so it stays.
        self.assertNotIn("-foffload-fp32-prec", translated)
        self.assertIn("-ffp-contract=off", translated)
        self.assertNotIn("-fsycl", translated)
        self.assertNotIn("-fno-sycl-rdc", translated)
        # ADR-1590: the compression level is icpx-only as well.
        self.assertNotIn("--offload-compress", translated)
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

    def test_drops_the_build_depfile_arguments(self) -> None:
        # PR #1764: the SYCL TUs are compiled with -MD -MF <depfile>.
        generator = load_generator()
        translated = generator.clang_tidy_command(
            "icpx -fsycl -pedantic -Isrc -MD -MF src/unit.o.d ../unit.cpp -o src/unit.o"
        )
        self.assertNotIn("-MD", translated)
        self.assertNotIn("-MF", translated)
        self.assertNotIn("unit.o.d", translated)
        self.assertIn("-Isrc", translated)
        self.assertIn("../unit.cpp", translated)


ICPX = "/opt/intel/oneapi/compiler/2026.0/bin/icpx"
# A target without a depfile (the test probes) and one with (the feature TUs).
PLAIN_STATEMENT = f"""build test/probe.o: CUSTOM_COMMAND ../core/test/probe.cpp | ../core/x.h {ICPX}
 COMMAND = {ICPX} -pedantic -c -fsycl ../core/test/probe.cpp -o test/probe.o
 description = Generating$ test/probe$ with$ a$ custom$ command

"""
DEPFILE_STATEMENT = f"""build src/twin.o: CUSTOM_COMMAND_DEP ../core/src/./feature/sycl/twin.cpp | {ICPX}
 DEPFILE = src/twin.o.d
 DEPFILE_UNQUOTED = src/twin.o.d
 COMMAND = {ICPX} -pedantic -c -fsycl -Isrc -MD -MF src/twin.o.d ../core/src/./feature/sycl/twin.cpp -o src/twin.o
 description = Generating$ src/sycl_feature_twin$ with$ a$ custom$ command

"""
# A device link reads an object, not a source: it is not a translation unit.
LINK_STATEMENT = f"""build test/probe_link.obj: CUSTOM_COMMAND test/probe.o | {ICPX}
 COMMAND = {ICPX} -fsycl -fsycl-link test/probe.o -o test/probe_link.obj

"""


class ParseNinjaTests(unittest.TestCase):
    def _parse(self, ninja: str, generator: ModuleType | None = None) -> list[dict[str, str]]:
        generator = generator or load_generator()
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp) / "build-sycl"
            build_dir.mkdir()
            path = build_dir / "build.ninja"
            path.write_text(ninja, encoding="utf-8")
            entries: list[dict[str, str]] = generator.parse_ninja_sycl_commands(path)
            return entries

    def test_parses_targets_with_and_without_a_depfile(self) -> None:
        entries = self._parse(PLAIN_STATEMENT + DEPFILE_STATEMENT + LINK_STATEMENT)
        self.assertEqual(
            sorted(Path(entry["file"]).name for entry in entries), ["probe.cpp", "twin.cpp"]
        )
        twin = next(entry for entry in entries if entry["file"].endswith("twin.cpp"))
        self.assertTrue(twin["command"].startswith("clang++ "))
        self.assertNotIn("-MF", twin["command"])
        self.assertIn("-Isrc", twin["command"])

    def test_statement_under_an_unknown_rule_is_an_error(self) -> None:
        # The failure this guards: ninja renamed the rule (CUSTOM_COMMAND_DEP)
        # and the lane measured no SYCL feature TU while reporting clean.
        generator = load_generator()
        renamed = DEPFILE_STATEMENT.replace("CUSTOM_COMMAND_DEP", "CUSTOM_COMMAND_NEXT")
        with self.assertRaises(generator.UnparsedSyclCommandError):
            self._parse(PLAIN_STATEMENT + renamed, generator)

    def test_main_reports_the_error(self) -> None:
        generator = load_generator()
        renamed = DEPFILE_STATEMENT.replace("CUSTOM_COMMAND_DEP", "CUSTOM_COMMAND_NEXT")
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp)
            (build_dir / "build.ninja").write_text(renamed, encoding="utf-8")
            (build_dir / "compile_commands.json").write_text("[]\n", encoding="utf-8")
            self.assertEqual(generator.main(["gen", str(build_dir)]), 1)
            self.assertEqual(
                (build_dir / "compile_commands.json").read_text(encoding="utf-8"), "[]\n"
            )


if __name__ == "__main__":
    unittest.main()
